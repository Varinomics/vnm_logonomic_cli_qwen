#include "qwen_observer.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_lifecycle.h"
#include "qwen_metadata_file_p.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace qwen {
namespace {

namespace addon       = logonomic::cli_addon;
namespace json        = logonomic::cli_addon::json;
namespace fs          = std::filesystem;
namespace observation = vnm::filesystem_observation;
namespace path_name   = vnm::path_name;

using Clock = std::chrono::steady_clock;

constexpr std::size_t k_metadata_bound = 1024 * 1024;
constexpr std::size_t k_session_bound  = 256;
constexpr std::size_t k_retained_bound = 4096;
constexpr std::size_t k_active_bound   = 128;

class Observation_failure final : public std::runtime_error
{
public:
    explicit Observation_failure(const char* reason)
    :
        std::runtime_error(reason)
    {}
};

struct Cursor
{
    std::string key;
    std::uint64_t sequence = 0;
};

Cursor cursor_for(const addon::Session_correlation& correlation, std::string_view value)
{
    if (value.empty()) {
        return {correlation.transition_nonce, 0};
    }
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || value.substr(0, separator) != correlation.transition_nonce) {
        throw Observation_failure("invalid_observation_cursor");
    }
    Cursor cursor{correlation.transition_nonce, 0};
    const auto suffix = value.substr(separator + 1);
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), cursor.sequence);
    if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
        std::to_string(cursor.sequence) != suffix)
    {
        throw Observation_failure("invalid_observation_cursor");
    }
    return cursor;
}

std::string cursor_text(const Cursor& cursor)
{
    return cursor.key + ':' + std::to_string(cursor.sequence);
}

std::string filename_component(const std::string& value)
{
    // The native JavaScript regexp replaces UTF-16 code units, not UTF-8 bytes.
    std::string result;
    for (const QChar unit : QString::fromStdString(value)) {
        const auto byte = unit.unicode();
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
        result.push_back(allowed ? (char)byte : '_');
    }
    return result;
}

fs::path child_directory(const Json& body, const std::string& session)
{
    const auto transcript = addon::path_from_utf8(json::text(body, "transcript_path"));
    if (!transcript.is_absolute() || transcript.filename() != addon::path_from_utf8(session + ".jsonl") ||
        transcript.parent_path().filename() != "chats")
    {
        return {};
    }
    return transcript.parent_path().parent_path() / "subagents" / filename_component(session);
}

struct Metadata
{
    std::uint64_t resume_count = 0;
    std::string created_at;
    std::string updated_at;
    bool terminal = false;
};

struct Metadata_read
{
    std::optional<Metadata> snapshot;
    bool retry = false;
};

Metadata_read read_metadata(
    const fs::path& path, const std::string& session, const std::string& agent)
{
    std::error_code error;
    const auto size = fs::file_size(native_io_path(path), error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return {};
    }
    if (error || size > k_metadata_bound) {
        return {std::nullopt, true};
    }
    auto input = detail::open_metadata_file(native_io_path(path));
    if (!input) {
        // Atomic replacement may remove the path between stat and open.
        if (!fs::exists(native_io_path(path), error) && !error) {
            return {};
        }
        return {std::nullopt, true};
    }
    std::string bytes(k_metadata_bound + 1, '\0');
    bytes.resize(std::fread(bytes.data(), 1, bytes.size(), input.get()));
    if (std::ferror(input.get()) || bytes.size() > k_metadata_bound) {
        return {std::nullopt, true};
    }
    addon::Json_parse_limits limits;
    limits.max_string_bytes = k_metadata_bound;
    const auto value = addon::parse_json(bytes, limits, nullptr);
    if (!value || !value->is_object() || json::text(*value, "parentSessionId") != session ||
        json::text(*value, "agentId") != agent)
    {
        return {std::nullopt, true};
    }
    const auto* count = value->find("resumeCount");
    if (!count || !count->is_number() || !std::isfinite(count->as_number()) ||
        count->as_number() < 0 || count->as_number() > 9007199254740991.0 ||
        std::floor(count->as_number()) != count->as_number() ||
        !json::bounded_string(json::member(*value, "createdAt"), 160) ||
        !json::bounded_string(json::member(*value, "lastUpdatedAt"), 160))
    {
        return {std::nullopt, true};
    }
    const auto status = json::text(*value, "status");
    const bool terminal = status == "completed" || status == "failed" ||
                          status == "cancelled" || status == "paused";
    if (status != "running" && !terminal) {
        return {std::nullopt, true};
    }
    return {Metadata{(std::uint64_t)count->as_number(), json::text(*value, "createdAt"),
        json::text(*value, "lastUpdatedAt"), terminal}, false};
}

struct Child
{
    std::string native_id;
    std::string run_id;
    std::string label;
    fs::path path;
    std::optional<Metadata> initial;
    bool initial_pending = false;
    std::optional<std::uint64_t> resume_count;
    std::string created_at;
    bool active = true;
};

struct Binding
{
    std::string session;
    std::string nonce;
    fs::path directory;
    std::map<std::string, Child> children;
    std::set<std::string> starts;
    std::deque<addon::Lifecycle_observation> queue;
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence  = 0;
    std::size_t active_children = 0;
    observation::Subscription subscription;
    std::shared_ptr<observation::Wait_channel> channel;
    QStringList watched_files;
    QStringList watched_directories;
    std::string failure;
    std::optional<std::string> final_cursor;
    bool observing = false;
    bool watch_dirty = true;
    bool closed = false;
    bool retired = false;
};

void stop_observation(Binding& binding)
{
    binding.subscription.reset();
    if (binding.channel) {
        binding.channel->cancel();
    }
    binding.channel.reset();
    binding.watched_files.clear();
    binding.watched_directories.clear();
}

void enqueue(Binding& binding, addon::Lifecycle_observation event)
{
    if (binding.queue.size() == k_retained_bound) {
        binding.failure = "subagent_observation_queue_capacity_exhausted";
        throw Observation_failure("subagent_observation_queue_capacity_exhausted");
    }
    ++binding.next_sequence;
    binding.queue.push_back(std::move(event));
}

void child_event(Binding& binding, Child& child, bool started)
{
    addon::Lifecycle_observation event;
    event.provider_session_id = binding.session;
    event.provider_event_name = started ? "SubagentStart" : "SubagentFinished";
    event.provider_event_id   = child.run_id + (started ? ":start" : ":stop");
    event.agent_id            = child.run_id;
    event.agent_label         = child.label;
    event.subagent_transition = started ? addon::Subagent_transition::STARTED
                                       : addon::Subagent_transition::STOPPED;
    // Per-child resume counters are not the host's session-wide sequence.
    enqueue(binding, std::move(event));
    child.active = started;
    binding.watch_dirty = true;
    if (started) {
        ++binding.active_children;
    }
    else {
        --binding.active_children;
    }
}

void accept_metadata(Binding& binding, Child& child, const Metadata& metadata)
{
    if (!child.active) {
        return;
    }
    if (child.initial_pending) {
        // A failed read at START leaves the earlier snapshot unknown. The
        // first recovered terminal record cannot establish a new completion.
        child.initial         = metadata;
        child.initial_pending = false;
    }
    if (!child.created_at.empty() && child.created_at != metadata.created_at) {
        return;
    }
    if (child.resume_count && metadata.resume_count != *child.resume_count) {
        // Qwen persists a resume before firing its START hook. Metadata alone
        // cannot admit that attempt into this live Logonomic owner.
        return;
    }
    child.created_at = metadata.created_at;
    if (!child.resume_count) {
        child.resume_count = metadata.resume_count;
    }
    if (child.initial && child.initial->terminal && metadata.terminal &&
        child.initial->updated_at == metadata.updated_at)
    {
        // A failed best-effort resume write can leave the previous run's
        // terminal sidecar at START. It is not terminal evidence for this run.
        return;
    }
    if (metadata.terminal) {
        child_event(binding, child, false);
    }
}

bool scan(Binding& binding)
{
    bool complete = true;
    for (auto& [id, child] : binding.children) {
        (void)id;
        if (!child.active) {
            continue;
        }
        const auto metadata = read_metadata(child.path, binding.session, child.native_id);
        complete &= !metadata.retry;
        if (metadata.snapshot) {
            accept_metadata(binding, child, *metadata.snapshot);
        }
    }
    return complete;
}

bool directory_exists(const fs::path& path)
{
    std::error_code error;
    const bool exists = fs::is_directory(native_io_path(path), error);
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
        throw Observation_failure("subagent_directory_unavailable");
    }
    return exists;
}

QString watch_path(const fs::path& path)
{
    return path_name::normalized(QString::fromStdString(path_string(path)));
}

bool update_observation(
    Binding& binding, observation::Service& service, const observation::Configuration& configuration)
{
    if (binding.directory.empty()) {
        binding.watch_dirty = false;
        return false;
    }
    QStringList files;
    QStringList directories;
    fs::path frontier = binding.directory;
    while (!directory_exists(frontier.parent_path()) && frontier.parent_path() != frontier) {
        frontier = frontier.parent_path();
    }
    files.append(watch_path(frontier));
    if (directory_exists(binding.directory)) {
        directories.append(watch_path(binding.directory));
        for (const auto& [id, child] : binding.children) {
            (void)id;
            if (child.active) {
                files.append(watch_path(child.path));
            }
        }
    }
    files.removeDuplicates();
    files.sort();
    if (binding.channel && files == binding.watched_files && directories == binding.watched_directories) {
        binding.watch_dirty = false;
        return false;
    }
    observation::Subscription_options options;
    options.mode         = observation::Watch_mode::DIRECT;
    options.interval     = configuration.general_interval;
    options.policy_epoch = std::make_shared<path_name::Policy_epoch>();
    options.exact_files  = files;
    QList<path_name::Key> keys;
    for (const auto& path : files) {
        keys.append(options.policy_epoch->key(path));
    }
    for (const auto& directory : directories) {
        options.directories.append({directory, 0});
    }
    options.classifier = [keys](const observation::Event& event) {
        return std::any_of(keys.cbegin(), keys.cend(), [&](const auto& key) {
            return path_name::matches_key(event.path, key) || path_name::matches_key(event.old_path, key);
        });
    };
    auto channel = std::make_shared<observation::Wait_channel>();
    auto subscription = service.subscribe(std::move(options), channel);
    stop_observation(binding);
    binding.subscription        = std::move(subscription);
    binding.channel             = std::move(channel);
    binding.watched_files       = std::move(files);
    binding.watched_directories = std::move(directories);
    binding.watch_dirty = false;
    return true;
}

bool observation_covered(const Binding& binding)
{
    for (const auto& file : binding.watched_files) {
        if (!binding.subscription.covers_directory(QFileInfo(file).absolutePath())) {
            return false;
        }
    }
    for (const auto& directory : binding.watched_directories) {
        if (!binding.subscription.covers_directory(directory)) {
            return false;
        }
    }
    return true;
}

} // namespace

struct Lifecycle_observer::State
{
    std::shared_ptr<observation::Service> service;
    observation::Configuration configuration;
    std::mutex mutex;
    // Null values are reservations; only authenticated classification binds them.
    std::map<std::string, std::shared_ptr<Binding>> sessions;
};

Lifecycle_observer::Lifecycle_observer()
:
    Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(
    std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)
:
    m_state(std::make_unique<State>())
{
    if (!service) {
        throw std::invalid_argument("Qwen observer requires a process observation service");
    }
    configuration.validate();
    m_state->service       = std::move(service);
    m_state->configuration = configuration;
}

Lifecycle_observer::~Lifecycle_observer() = default;

addon::Classify_event_result Lifecycle_observer::classify(const addon::Classify_event_request& request)
{
    auto result = qwen::classify_event(request);
    const auto session = json::text(request.event.body, "session_id");
    const auto& name = request.event.name;
    std::lock_guard lock(m_state->mutex);
    if (result.classification == addon::Classification::TRANSITION && name == "SessionStart") {
        const auto found = m_state->sessions.find(request.transition_nonce);
        if (found == m_state->sessions.end()) {
            throw Observation_failure("correlation_not_reserved");
        }
        const auto directory = child_directory(request.event.body, session);
        if (found->second) {
            if (found->second->session != session || found->second->directory != directory) {
                throw Observation_failure("subagent_session_already_bound");
            }
            return result;
        }
        auto binding = std::make_shared<Binding>();
        binding->session   = session;
        binding->nonce     = request.transition_nonce;
        binding->directory = directory;
        found->second = std::move(binding);
        return result;
    }
    const auto& current = request.ledger.current_transition;
    if (!current || request.ledger.current_epoch == 0 || current->provider_session_id != session) {
        return result;
    }
    const auto found = m_state->sessions.find(current->nonce);
    if (found == m_state->sessions.end() || !found->second || found->second->session != session) {
        return result;
    }
    auto& binding = *found->second;
    if (binding.closed || binding.retired) {
        return {};
    }
    if (name == "SessionEnd" && result.observations.size() == 1 &&
        result.observations.front().session_transition == addon::Session_transition::ENDED)
    {
        // The root's terminal fact follows already admitted child facts. Its
        // final cursor, rather than a watch timeout, releases this correlation.
        enqueue(binding, std::move(result.observations.front()));
        result.observations.clear();
        result.classification = addon::Classification::IGNORED;
        binding.closed = true;
        stop_observation(binding);
        return result;
    }
    if (name != "SubagentStart") {
        return result;
    }
    if (!binding.failure.empty()) {
        throw Observation_failure("subagent_observation_unavailable");
    }
    const auto& body = request.event.body;
    if (!json::bounded_string(json::member(body, "agent_id"), 256) ||
        !json::bounded_string(json::member(body, "event_id"), 256) ||
        !json::bounded_string(json::member(body, "agent_type"), 160))
    {
        throw Observation_failure("subagent_hook_identity_invalid");
    }
    const auto event_id = json::text(body, "event_id");
    if (binding.starts.contains(event_id)) {
        return result;
    }
    if (binding.directory.empty()) {
        throw Observation_failure("subagent_store_unavailable");
    }
    const auto agent = json::text(body, "agent_id");
    auto previous = binding.children.find(agent);
    if (binding.starts.size() == k_retained_bound ||
        (previous == binding.children.end() && binding.children.size() == k_retained_bound) ||
        (binding.active_children == k_active_bound &&
            (previous == binding.children.end() || !previous->second.active)))
    {
        throw Observation_failure("subagent_identity_capacity_exhausted");
    }
    Child child;
    child.native_id = agent;
    child.run_id = "qwen:" + addon::to_hex(addon::Sha256::digest(binding.nonce + ':' + event_id));
    child.label = json::text(body, "agent_type");
    child.path = binding.directory / ("agent-" + filename_component(agent) + ".meta.json");
    const auto initial    = read_metadata(child.path, session, agent);
    child.initial         = initial.snapshot;
    child.initial_pending = initial.retry;
    if (child.initial) {
        child.created_at = child.initial->created_at;
        child.resume_count = child.initial->resume_count;
    }
    if (previous != binding.children.end() && previous->second.active) {
        // A native re-entry START means the prior execution has yielded. Stop
        // hooks themselves are blockable and never enter this path.
        child_event(binding, previous->second, false);
    }
    binding.starts.insert(event_id);
    auto inserted = binding.children.insert_or_assign(agent, std::move(child)).first;
    child_event(binding, inserted->second, true);
    if (binding.channel) {
        binding.subscription.request_rescan(QStringLiteral("Qwen child start admitted"));
    }
    return result;
}

void Lifecycle_observer::handle(addon::Invocation_context& context)
{
    try {
        addon::Contract_failure failure;
        const auto& invocation = context.request();
        if (invocation.capability.capability_id == "lifecycle-correlation-lifetime") {
            if (invocation.operation == addon::k_lifecycle_correlation_lifetime_reserve_operation) {
                const auto request = addon::decode_lifecycle_correlation_reserve_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                std::lock_guard lock(m_state->mutex);
                if (context.cancel_requested()) {
                    context.fail(addon::Result_status::CANCELLED, "cancelled");
                    return;
                }
                if (!m_state->sessions.contains(request->correlation.transition_nonce)) {
                    if (m_state->sessions.size() == k_session_bound) {
                        throw Observation_failure("subagent_session_capacity_exhausted");
                    }
                    m_state->sessions.emplace(request->correlation.transition_nonce, nullptr);
                }
                context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            }
            else
            if (invocation.operation == addon::k_lifecycle_correlation_lifetime_retire_operation) {
                const auto request = addon::decode_lifecycle_correlation_retire_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                std::lock_guard lock(m_state->mutex);
                const auto found = m_state->sessions.find(request->correlation.transition_nonce);
                if (found != m_state->sessions.end()) {
                    const auto binding = found->second;
                    if (request->disposition == addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED &&
                        (!binding || !binding->closed || !binding->final_cursor ||
                            binding->final_cursor != request->consumed_cursor))
                    {
                        throw Observation_failure("terminal_cursor_not_consumed");
                    }
                    if (binding) {
                        binding->retired = true;
                        stop_observation(*binding);
                        binding->children.clear();
                        binding->starts.clear();
                        binding->queue.clear();
                        binding->directory.clear();
                    }
                    m_state->sessions.erase(found);
                }
                context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
            }
            else {
                context.reject_unknown_operation();
            }
            return;
        }
        if (invocation.operation == "classify_event") {
            if (invocation.capability.capability_id == addon::k_attention_kind_capability_id) {
                const auto request = addon::decode_attention_classification_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                auto result = qwen::classify_attention(*request);
                result.base = classify(request->base);
                if (result.base.observations.size() != 1 ||
                    result.base.observations.front().attention_transition != addon::Attention_transition::REQUESTED)
                {
                    result.attention_kind.reset();
                }
                context.succeed(addon::encode_attention_classification_result(result));
            }
            else {
                const auto request = addon::decode_classify_event_request(invocation.payload, &failure);
                if (!request) {
                    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                    return;
                }
                context.succeed(addon::encode_classify_event_result(classify(*request)));
            }
            return;
        }
        if (invocation.operation == "poll_observations") {
            const auto request = addon::decode_poll_observations_request(invocation.payload, &failure);
            if (!request) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (!request->correlation) {
                context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
                return;
            }
            const auto cursor = cursor_for(*request->correlation, request->resume_cursor);
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->sessions.find(cursor.key);
            if (found == m_state->sessions.end() || !found->second) {
                throw Observation_failure("unknown_observation_cursor");
            }
            auto& binding = *found->second;
            if (!binding.failure.empty()) {
                context.fail(addon::Result_status::FAILED, binding.failure);
                return;
            }
            if (cursor.sequence < binding.first_sequence || cursor.sequence > binding.next_sequence) {
                throw Observation_failure("invalid_observation_cursor");
            }
            while (binding.first_sequence < cursor.sequence) {
                binding.queue.pop_front();
                ++binding.first_sequence;
            }
            addon::Poll_observations_result result;
            const auto count = std::min(request->limit, binding.queue.size());
            result.observations.assign(binding.queue.begin(), binding.queue.begin() + (std::ptrdiff_t)count);
            result.resume_cursor = cursor_text({cursor.key, cursor.sequence + count});
            result.more = count < binding.queue.size();
            if (binding.closed && !result.more) {
                binding.final_cursor = result.resume_cursor;
            }
            context.succeed(addon::encode_poll_observations_result(result));
            return;
        }
        if (invocation.operation != "observe") {
            context.reject_unknown_operation();
            return;
        }
        const auto request = addon::decode_observe_request(invocation.payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (!request->correlation) {
            context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
            return;
        }
        const auto cursor = cursor_for(*request->correlation, request->resume_cursor);
        std::shared_ptr<Binding> binding;
        std::uint64_t window_sequence = 0;
        {
            std::lock_guard lock(m_state->mutex);
            const auto found = m_state->sessions.find(cursor.key);
            if (found == m_state->sessions.end() || !found->second ||
                found->second->session != request->session.provider_session_id)
            {
                context.fail(addon::Result_status::UNAVAILABLE, "session_start_required");
                return;
            }
            binding = found->second;
            if (binding->observing || cursor.sequence > binding->next_sequence) {
                throw Observation_failure("invalid_observation_cursor");
            }
            if (!binding->directory.empty() && !binding->closed) {
                const auto spelling = watch_path(binding->directory);
                path_name::Policy_epoch policy;
                for (const auto& [nonce, other] : m_state->sessions) {
                    (void)nonce;
                    if (other && other != binding && !other->closed && !other->directory.empty() &&
                        policy.same(spelling, watch_path(other->directory)))
                    {
                        throw Observation_failure("subagent_session_already_bound");
                    }
                }
            }
            // Polling alone acknowledges queue entries. The final drain of a
            // completed watch may advance consumption beyond its watch cursor.
            window_sequence = std::max(cursor.sequence, binding->first_sequence);
            binding->observing = true;
        }
        std::string error;
        const auto deadline = Clock::now() + std::chrono::milliseconds(request->window_ms);
        auto reported_sequence = window_sequence;
        try {
            while (!context.cancel_requested() && Clock::now() < deadline) {
                std::shared_ptr<observation::Wait_channel> channel;
                {
                    std::lock_guard lock(m_state->mutex);
                    if (binding->retired) {
                        break;
                    }
                    if (!binding->failure.empty()) {
                        error = binding->failure;
                        break;
                    }
                    if (binding->next_sequence > reported_sequence) {
                        reported_sequence = binding->next_sequence;
                        const auto available = reported_sequence - window_sequence;
                        context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
                    }
                    if (binding->closed) {
                        break;
                    }
                    if (!binding->channel || binding->watch_dirty) {
                        update_observation(*binding, *m_state->service, m_state->configuration);
                    }
                    channel = binding->channel;
                }
                if (!channel) {
                    // Recording can be disabled without disabling root hooks.
                    // This wait bounds cancellation only; it performs no scans.
                    context.wait_for_cancel(100);
                    continue;
                }
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                auto delivery = channel->wait(std::min(remaining, std::chrono::milliseconds(100)));
                if (!delivery) {
                    continue;
                }
                {
                    std::lock_guard lock(m_state->mutex);
                    if (delivery->valid() && !binding->retired && !binding->closed && binding->channel == channel) {
                        const bool replaced = update_observation(
                            *binding, *m_state->service, m_state->configuration);
                        if (!replaced) {
                            if (scan(*binding) && observation_covered(*binding)) {
                                binding->subscription.acknowledge(delivery->batch.capture);
                            }
                            else {
                                // The shared service paces retries at the subscription's
                                // interval. One unreadable child cannot poison root hooks
                                // or prevent other children from publishing their facts.
                                binding->subscription.request_rescan(QStringLiteral("Qwen child evidence incomplete"));
                            }
                        }
                    }
                }
                delivery->completion.complete();
            }
        }
        catch (const Observation_failure& exception) {
            error = exception.what();
        }
        catch (const std::exception&) {
            error = "subagent_observation_failed";
        }
        std::lock_guard lock(m_state->mutex);
        binding->observing = false;
        if (binding->retired || context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, binding->retired ? "correlation_retired" : "cancelled");
        }
        else
        if (!error.empty()) {
            binding->failure = error;
            stop_observation(*binding);
            context.fail(addon::Result_status::FAILED, error);
        }
        else {
            context.succeed(addon::encode_observe_result({cursor_text({cursor.key, binding->next_sequence})}));
        }
    }
    catch (const Observation_failure& exception) {
        context.fail(addon::Result_status::FAILED, exception.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "subagent_observation_failed");
    }
}

} // namespace qwen
