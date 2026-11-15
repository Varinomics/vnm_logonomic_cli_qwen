#include "qwen_observer.h"
#include "qwen_json.h"
#include "qwen_launcher.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <QSaveFile>
#include <QString>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace addon       = logonomic::cli_addon;
namespace json        = logonomic::cli_addon::json;
namespace fs          = std::filesystem;
namespace observation = vnm::filesystem_observation;

using Json = addon::Json_value;

constexpr const char* k_session = "f21de9a6-8bd3-4f26-b562-18e11626d075";
constexpr const char* k_created = "2026-11-15T12:00:00.000Z";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancel.load(); }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return cancel_requested();
    }
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t>, std::string) override
    {
        m_available = completed;
    }
    void succeed(Json result) override { m_result = std::move(result); }
    void fail(addon::Result_status status, std::string reason, std::string) override
    {
        m_status  = status;
        m_failure = std::move(reason);
    }
    bool finished() const override { return m_result.has_value() || !m_failure.empty(); }

    addon::Invoke_request m_request;
    std::optional<Json> m_result;
    addon::Result_status m_status = addon::Result_status::SUCCESS;
    std::string m_failure;
    std::atomic<bool> m_cancel = false;
    std::atomic<std::uint64_t> m_available = 0;
};

bool reserve(qwen::Lifecycle_observer& observer, const std::string& nonce)
{
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation   = "reserve";
    context.m_request.payload = addon::encode_lifecycle_correlation_reserve_request({{nonce}});
    observer.handle(context);
    return context.m_result.has_value();
}

bool retire(qwen::Lifecycle_observer& observer, const std::string& nonce,
    std::optional<std::string> cursor = std::nullopt)
{
    addon::Lifecycle_correlation_retire_request request;
    request.correlation = {nonce};
    request.disposition = cursor ? addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED
                                 : addon::Lifecycle_correlation_retire_disposition::ABANDONED;
    request.consumed_cursor = std::move(cursor);
    Context context;
    context.m_request.capability = {"lifecycle-correlation-lifetime", 1};
    context.m_request.operation   = "retire";
    context.m_request.payload = addon::encode_lifecycle_correlation_retire_request(request);
    observer.handle(context);
    return context.m_result.has_value();
}

std::optional<addon::Poll_observations_result> poll(
    qwen::Lifecycle_observer& observer, const std::string& nonce,
    std::string cursor = {}, std::size_t limit = 16)
{
    addon::Poll_observations_request request;
    request.correlation   = addon::Session_correlation{nonce};
    request.resume_cursor = std::move(cursor);
    request.limit         = limit;
    Context context;
    context.m_request.capability = {"lifecycle-events", 2};
    context.m_request.operation   = "poll_observations";
    context.m_request.payload = addon::encode_poll_observations_request(request);
    observer.handle(context);
    addon::Contract_failure failure;
    return context.m_result
        ? addon::decode_poll_observations_result(*context.m_result, limit, &failure)
        : std::nullopt;
}

bool wait_until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void write_bytes(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    QSaveFile output(QString::fromStdString(qwen::path_string(path)));
    require(output.open(QIODevice::WriteOnly), "open native metadata fixture");
    require(output.write(bytes.data(), (qint64)bytes.size()) == (qint64)bytes.size(),
        "write complete native metadata fixture");
    require(output.commit(), "atomically replace native metadata fixture");
}

observation::Configuration test_configuration()
{
    observation::Configuration result;
    result.general_interval = std::chrono::milliseconds(25);
    return result;
}

class Fixture final
{
public:
    Fixture()
    :
        m_root(fs::temp_directory_path() / ("qwen-observer-" + addon::random_nonce_hex())),
        m_service(std::make_shared<observation::Service>()),
        m_observer(m_service, test_configuration()),
        m_nonce(addon::random_nonce_hex())
    {
        fs::create_directories(m_root / "projects/workspace/chats");
        require(reserve(m_observer, m_nonce), "reserve authenticated root correlation");
        auto request = hook("SessionStart");
        request.ledger = {};
        request.transition_nonce = m_nonce;
        request.event.body.set("source", Json("startup"));
        request.event.body.set("transcript_path", Json(qwen::path_string(
            m_root / "projects/workspace/chats" / (std::string(k_session) + ".jsonl"))));
        const auto result = m_observer.classify(request);
        require(result.classification == addon::Classification::TRANSITION && result.observations.size() == 1,
            "native root START preserves root lifecycle classification");
    }

    ~Fixture()
    {
        retire(m_observer, m_nonce);
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    addon::Classify_event_request hook(const std::string& name, const std::string& agent = {}) const
    {
        addon::Classify_event_request request;
        request.event.name = name;
        request.event.body = json::object({
            {"session_id", Json(k_session)}, {"event_id", Json(addon::random_nonce_hex())}});
        request.ledger.current_epoch = 1;
        request.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::START, k_session, m_nonce};
        if (!agent.empty()) {
            request.event.body.set("agent_id", Json(agent));
            request.event.body.set("agent_type", Json("code-reviewer"));
        }
        return request;
    }

    fs::path metadata_path(const std::string& filename_agent) const
    {
        return m_root / "projects/workspace/subagents" / k_session /
            ("agent-" + filename_agent + ".meta.json");
    }

    void metadata(const std::string& agent, const std::string& status,
        unsigned resume_count, const std::string& updated_at,
        const std::string& filename_agent = {}) const
    {
        // Oracle: Qwen AgentMeta and the foreground/background final writers.
        // Terminal metadata is written after the blockable SubagentStop loop.
        const auto record = json::object({
            {"agentId", Json(agent)}, {"parentSessionId", Json(k_session)},
            {"parentAgentId", Json()}, {"toolUseId", Json("agent-tool-call")},
            {"agentType", Json("code-reviewer")}, {"createdAt", Json(k_created)},
            {"lastUpdatedAt", Json(updated_at)}, {"status", Json(status)},
            {"resumeCount", Json((double)resume_count)}});
        write_bytes(metadata_path(filename_agent.empty() ? agent : filename_agent),
            addon::serialize_json_canonical(record));
    }

    addon::Poll_observations_result await_page(std::size_t count, const std::string& cursor = {})
    {
        std::optional<addon::Poll_observations_result> result;
        require(wait_until([&] {
            result = poll(m_observer, m_nonce, cursor);
            return result && result->observations.size() == count;
        }), "native metadata must reach the bounded observation queue");
        return *result;
    }

    std::uint64_t await_capture(std::uint64_t after = 0)
    {
        std::uint64_t dispatches = 0;
        require(wait_until([&] {
            const auto stats = m_service->statistics();
            dispatches = stats.dispatches;
            return stats.dispatches > after && stats.outstanding == 0 &&
                stats.pending_registrations == 0 && stats.retained_events == 0;
        }), "filesystem observation must finish its native acquisition capture");
        return dispatches;
    }

    fs::path m_root;
    std::shared_ptr<observation::Service> m_service;
    qwen::Lifecycle_observer m_observer;
    std::string m_nonce;
};

class Observe_window final
{
public:
    explicit Observe_window(Fixture& fixture, std::string cursor = {})
    {
        addon::Observe_request request;
        request.session = {qwen::path_string(fixture.m_root), {"qwen"}, k_session};
        request.correlation   = addon::Session_correlation{fixture.m_nonce};
        request.resume_cursor = std::move(cursor);
        request.window_ms     = addon::limits::k_observe_window_ms;
        m_context.m_request.capability = {"lifecycle-events", 2};
        m_context.m_request.operation   = "observe";
        m_context.m_request.payload = addon::encode_observe_request(request);
        m_worker = std::async(std::launch::async, [&fixture, this] {
            fixture.m_observer.handle(m_context);
        });
    }

    ~Observe_window()
    {
        m_context.m_cancel = true;
        m_worker.wait();
    }

    Context m_context;
    std::future<void> m_worker;
};

void stopped_hooks_do_not_end_active_children()
{
    Fixture fixture;
    Observe_window window(fixture);
    const auto start = fixture.hook("SubagentStart", "review-a");
    require(fixture.m_observer.classify(start).observations.empty(),
        "child admission uses the ordered observation queue");
    fixture.metadata("review-a", "running", 0, k_created);
    const auto started = fixture.await_page(1);
    require(started.observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "authenticated native START opens the child span");
    const auto replay = poll(fixture.m_observer, fixture.m_nonce);
    require(replay && replay->observations.front().provider_event_id == started.observations.front().provider_event_id,
        "unconsumed child page replays the same semantic event");
    fixture.m_observer.classify(start);
    require(fixture.m_observer.classify(fixture.hook("SubagentStop", "review-a")).observations.empty(),
        "blockable SubagentStop supplies no terminal observation");
    const auto root_stop = fixture.m_observer.classify(fixture.hook("Stop"));
    require(root_stop.observations.size() == 1 &&
        root_stop.observations.front().phase_transition == addon::Phase_transition::AWAITING_INPUT,
        "root Stop retains its existing inferred phase evidence");
    const auto pending = poll(fixture.m_observer, fixture.m_nonce, started.resume_cursor);
    require(pending && pending->observations.empty(), "root idle and child Stop cannot end background work");
    fixture.metadata("review-a", "completed", 0, "2026-11-15T12:00:01.000Z");
    const auto stopped = fixture.await_page(1, started.resume_cursor);
    require(stopped.observations.front().subagent_transition == addon::Subagent_transition::STOPPED &&
        stopped.observations.front().agent_id == started.observations.front().agent_id,
        "post-gate terminal metadata closes exactly the admitted child attempt");
}

void fast_children_and_authenticated_inventory()
{
    Fixture fixture;
    fixture.metadata("unowned-child", "completed", 0, k_created);
    auto unauthenticated = fixture.hook("SubagentStart", "unowned-child");
    unauthenticated.ledger = {};
    fixture.m_observer.classify(unauthenticated);
    const auto start = fixture.hook("SubagentStart", "fast-child");
    fixture.m_observer.classify(start);
    fixture.metadata("fast-child", "failed", 0, "2026-11-15T12:00:01.000Z");
    Observe_window window(fixture);
    const auto page = fixture.await_page(2);
    require(page.observations[0].subagent_transition == addon::Subagent_transition::STARTED &&
        page.observations[1].subagent_transition == addon::Subagent_transition::STOPPED &&
        page.observations[0].agent_id == page.observations[1].agent_id,
        "a child finishing before the first scan keeps its paired START and STOP");
    for (const auto& event : page.observations) {
        require(event.provider_session_id == k_session && event.provider_sequence.empty() &&
            event.session_transition == addon::Session_transition::NONE &&
            event.phase_transition == addon::Phase_transition::NONE &&
            event.outcome_transition == addon::Outcome_transition::NONE,
            "child facts retain the root identity without changing root phase, outcome or sequence");
    }
    require(wait_until([&] { return window.m_context.m_available.load() >= 2; }),
        "queued children from before observe still wake the host");
    fixture.m_observer.classify(start);
    const auto empty = poll(fixture.m_observer, fixture.m_nonce, page.resume_cursor);
    require(empty && empty->observations.empty(), "helper retry cannot reopen a completed child attempt");
}

void resumes_do_not_reuse_stale_terminal_evidence()
{
    Fixture fixture;
    fixture.m_observer.classify(fixture.hook("SubagentStart", "resumable"));
    fixture.metadata("resumable", "completed", 0, "2026-11-15T12:00:01.000Z");
    Observe_window window(fixture);
    const auto first = fixture.await_page(2);
    fixture.metadata("resumable", "running", 1, "2026-11-15T12:00:02.000Z");
    fixture.m_observer.classify(fixture.hook("SubagentStart", "resumable"));
    const auto resumed = fixture.await_page(1, first.resume_cursor);
    require(resumed.observations.front().agent_id != first.observations.front().agent_id,
        "resuming a durable native agent opens a distinct authenticated attempt");
    fixture.metadata("resumable", "cancelled", 1, "2026-11-15T12:00:03.000Z");
    const auto cancelled = fixture.await_page(1, resumed.resume_cursor);
    require(cancelled.observations.front().subagent_transition == addon::Subagent_transition::STOPPED,
        "native cancelled metadata terminates the resumed attempt");

    // Qwen's best-effort resume patch can fail while the START hook still runs.
    const auto before_retry = fixture.await_capture();
    fixture.m_observer.classify(fixture.hook("SubagentStart", "resumable"));
    fixture.await_capture(before_retry);
    const auto retry = fixture.await_page(1, cancelled.resume_cursor);
    require(retry.observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "a new authenticated attempt is visible even with stale terminal metadata");
    const auto stale = poll(fixture.m_observer, fixture.m_nonce, retry.resume_cursor);
    require(stale && stale->observations.empty(), "the previous run's terminal snapshot is not this run's STOP");
    fixture.metadata("resumable", "paused", 1, "2026-11-15T12:00:04.000Z");
    const auto paused = fixture.await_page(1, retry.resume_cursor);
    require(paused.observations.front().agent_id == retry.observations.front().agent_id,
        "fresh terminal metadata can settle the attempt after a failed resume-count write");
}

void metadata_cannot_admit_another_attempt()
{
    Fixture fixture;
    fixture.metadata("agent-live", "running", 0, k_created);
    fixture.m_observer.classify(fixture.hook("SubagentStart", "agent-live"));
    Observe_window window(fixture);
    const auto first = fixture.await_page(1);
    const auto before_change = fixture.await_capture();
    fixture.metadata("agent-live", "completed", 1, "2026-11-15T12:00:01.000Z");
    fixture.await_capture(before_change);
    const auto unadmitted = poll(fixture.m_observer, fixture.m_nonce, first.resume_cursor);
    require(unadmitted && unadmitted->observations.empty(),
        "a different metadata generation cannot start or stop this owner's admitted attempt");
}

void directory_replacement_and_native_unicode_names()
{
    Fixture fixture;
    const std::string agent = "review-\xc3\xa9-\xf0\x9f\x94\x8d";
    fixture.m_observer.classify(fixture.hook("SubagentStart", agent));
    Observe_window window(fixture);
    fixture.metadata(agent, "running", 0, k_created, "review-_-__");
    const auto started = fixture.await_page(1);
    const auto directory = fixture.metadata_path("review-_-__").parent_path();
    fixture.await_capture();
    fs::rename(directory, directory.string() + "-departed");
    fixture.metadata(agent, "completed", 0, "2026-11-15T12:00:01.000Z", "review-_-__");
    const auto stopped = fixture.await_page(1, started.resume_cursor);
    require(stopped.observations.front().agent_id == started.observations.front().agent_id,
        "directory replacement reopens the exact native UTF-16-sanitized metadata name");
}

void terminal_page_and_retirement()
{
    Fixture fixture;
    fixture.m_observer.classify(fixture.hook("SubagentStart", "ending-child"));
    const auto ended = fixture.m_observer.classify(fixture.hook("SessionEnd"));
    require(ended.observations.empty(), "root END waits behind admitted child evidence");
    const auto first = poll(fixture.m_observer, fixture.m_nonce, {}, 1);
    require(first && first->more && first->observations.size() == 1 &&
        first->observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "the bounded first page contains the child before root END");
    const auto final = poll(fixture.m_observer, fixture.m_nonce, first->resume_cursor, 1);
    require(final && !final->more && final->observations.size() == 1 &&
        final->observations.front().session_transition == addon::Session_transition::ENDED,
        "root END occupies the final consumption page");
    require(!retire(fixture.m_observer, fixture.m_nonce, first->resume_cursor),
        "terminal retirement requires the exact final cursor");
    const auto replay = poll(fixture.m_observer, fixture.m_nonce, first->resume_cursor, 1);
    require(replay && replay->resume_cursor == final->resume_cursor,
        "terminal evidence remains replayable until its exact retirement");
    require(retire(fixture.m_observer, fixture.m_nonce, final->resume_cursor),
        "consuming the terminal cursor retires its complete ownership state");
    require(!poll(fixture.m_observer, fixture.m_nonce), "retired cursor cannot expose another session's observations");
    require(retire(fixture.m_observer, fixture.m_nonce), "repeated retirement is idempotent");
}

void watch_continuation_is_independent_of_consumption()
{
    // Oracle: lifecycle-events@2 keeps the watch continuation and poll
    // consumption cursors independent, including the drain between windows.
    Fixture fixture;
    Observe_window first_window(fixture);
    fixture.m_observer.classify(fixture.hook("SubagentStart", "consumed-during-window"));
    require(wait_until([&] {
        return first_window.m_context.m_available.load() > 0 ||
            first_window.m_worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    }), "the first watch must signal child evidence before the host polls it");
    if (first_window.m_context.m_available.load() == 0) {
        throw std::runtime_error("the first watch could not admit its request: " + first_window.m_context.m_failure);
    }
    const auto first = fixture.await_page(1);
    const auto consumed = poll(fixture.m_observer, fixture.m_nonce, first.resume_cursor);
    require(consumed && consumed->observations.empty(), "the first child is acknowledged during its watch window");

    fixture.m_observer.classify(fixture.hook("SubagentStart", "unread-at-completion"));
    require(first_window.m_worker.wait_for(std::chrono::milliseconds(
        addon::limits::k_observe_window_ms + 5000)) == std::future_status::ready,
        "an ordinary watch window must complete within its SDK grace period");
    if (!first_window.m_context.m_result.has_value() ||
        first_window.m_context.m_status != addon::Result_status::SUCCESS)
    {
        throw std::runtime_error("the first watch failed before normal completion: " + first_window.m_context.m_failure);
    }
    addon::Contract_failure failure;
    const auto watch = addon::decode_observe_result(*first_window.m_context.m_result, &failure);
    require(watch.has_value(), "the completed watch returns a valid continuation cursor");

    const auto unread = poll(fixture.m_observer, fixture.m_nonce, consumed->resume_cursor);
    require(unread && unread->observations.size() == 1,
        "watch completion must not acknowledge unread child observations");
    const auto drained = poll(fixture.m_observer, fixture.m_nonce, unread->resume_cursor);
    require(drained && drained->observations.empty(), "the completed window's unread page is acknowledged");

    // The host drains after completion. Evidence arriving in that drain can
    // advance consumption beyond even the watch's final sequence.
    fixture.m_observer.classify(fixture.hook("SubagentStart", "arrived-between-windows"));
    const auto between = poll(fixture.m_observer, fixture.m_nonce, drained->resume_cursor);
    require(between && between->observations.size() == 1, "the final drain includes newly admitted child evidence");
    const auto acknowledged = poll(fixture.m_observer, fixture.m_nonce, between->resume_cursor);
    require(acknowledged && acknowledged->observations.empty(), "the final drain advances only consumption");

    fixture.m_observer.classify(fixture.hook("SubagentStart", "next-window-child"));
    Observe_window next_window(fixture, watch->resume_cursor);
    require(wait_until([&] {
        return next_window.m_context.m_available.load() > 0 ||
            next_window.m_worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    }), "the next watch resumes or reports its failure");
    require(next_window.m_context.m_available.load() > 0,
        "watch continuation must survive acknowledged poll consumption from the previous window");
    const auto next = poll(fixture.m_observer, fixture.m_nonce, acknowledged->resume_cursor);
    require(next && next->observations.size() == 1 &&
        next->observations.front().subagent_transition == addon::Subagent_transition::STARTED,
        "resuming the watch preserves the independent poll position and new child evidence");
}

void cancellation_and_cursor_isolation()
{
    Fixture fixture;
    fixture.m_observer.classify(fixture.hook("SubagentStart", "cancelled-watch"));
    Observe_window window(fixture);
    require(wait_until([&] { return window.m_context.m_available.load() == 1; }),
        "the live observer reports its retained child");
    const auto other_nonce = addon::random_nonce_hex();
    require(!poll(fixture.m_observer, other_nonce, fixture.m_nonce + ":0"),
        "a cursor cannot cross authenticated correlation nonces");
    require(!poll(fixture.m_observer, fixture.m_nonce, fixture.m_nonce + ":2"),
        "a cursor cannot consume observations that do not exist");
    require(retire(fixture.m_observer, fixture.m_nonce), "host abandonment retires the live watch");
    require(window.m_worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "retirement wakes and joins the outstanding observation window");
    require(window.m_context.m_status == addon::Result_status::CANCELLED &&
        window.m_context.m_failure == "correlation_retired",
        "an abandoned subscription cannot publish a successful completion");
    require(wait_until([&] { return fixture.m_service->statistics().subscriptions == 0; }),
        "retirement releases native filesystem subscriptions");
}

void invalid_metadata_does_not_fabricate_completion()
{
    Fixture fixture;
    fixture.metadata("corrupt-child", "running", 0, k_created);
    fixture.m_observer.classify(fixture.hook("SubagentStart", "corrupt-child"));
    Observe_window window(fixture);
    fixture.await_page(1);
    write_bytes(fixture.metadata_path("corrupt-child"), "{\"agentId\":");
    require(window.m_worker.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "an invalid published sidecar fails native child observation");
    require(window.m_context.m_status == addon::Result_status::FAILED &&
        window.m_context.m_failure == "subagent_metadata_identity_invalid",
        "corrupt metadata reports failure rather than a fabricated STOP");
    require(!poll(fixture.m_observer, fixture.m_nonce), "loss cannot masquerade as an empty successful inventory");
}

} // namespace

int main()
{
    try {
        watch_continuation_is_independent_of_consumption();
        stopped_hooks_do_not_end_active_children();
        fast_children_and_authenticated_inventory();
        resumes_do_not_reuse_stale_terminal_evidence();
        metadata_cannot_admit_another_attempt();
        directory_replacement_and_native_unicode_names();
        terminal_page_and_retirement();
        cancellation_and_cursor_isolation();
        invalid_metadata_does_not_fabricate_completion();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
