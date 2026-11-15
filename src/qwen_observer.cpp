#include "qwen_observer.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_lifecycle.h"
#include "qwen_metadata_file_p.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/filesystem_lifecycle_observer.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace qwen {
namespace {

namespace addon       = logonomic::cli_addon;
namespace json        = logonomic::cli_addon::json;
namespace fs          = std::filesystem;
namespace observation = vnm::filesystem_observation;
namespace path_name   = vnm::path_name;

constexpr std::size_t k_metadata_bound = 1024 * 1024;
constexpr std::size_t k_retained_bound = 4096;

class Observation_failure final : public std::runtime_error
{
public:
    explicit Observation_failure(const char* reason)

    :
        std::runtime_error(reason)
    {}
};

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

struct Binding final : addon::Filesystem_lifecycle_binding
{
    fs::path directory;
    std::map<std::string, Child> children;
    std::set<std::string> starts;

    observation::Subscription_options interests(const addon::Filesystem_lifecycle_bindings&) override;
    bool reconcile(addon::Invocation_context*) override;
    void check_owner(const addon::Filesystem_lifecycle_bindings& bindings) override;
    void release() override
    {
        directory.clear();
        children.clear();
        starts.clear();
    }
};

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
    if (binding.publish(std::move(event))) {
        child.active = started;
        binding.watch_dirty = true;
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
    return exists;
}

QString watch_path(const fs::path& path)
{
    return path_name::normalized(QString::fromStdString(path_string(path)));
}

bool Binding::reconcile(addon::Invocation_context*)
{
    return scan(*this);
}

observation::Subscription_options Binding::interests(const addon::Filesystem_lifecycle_bindings&)
{
    if (directory.empty()) {
        return {};
    }
    QStringList files;
    QStringList directories;
    fs::path frontier = directory;
    while (!directory_exists(frontier.parent_path()) && frontier.parent_path() != frontier) {
        frontier = frontier.parent_path();
    }
    files.append(watch_path(frontier));
    if (directory_exists(directory)) {
        directories.append(watch_path(directory));
        for (const auto& [id, child] : children) {
            (void)id;
            if (child.active) {
                files.append(watch_path(child.path));
            }
        }
    }
    return addon::lifecycle_file_interests(std::move(files), std::move(directories));
}

void Binding::check_owner(const addon::Filesystem_lifecycle_bindings& bindings)
{
    if (directory.empty() || closed) {
        return;
    }
    const auto spelling = watch_path(directory);
    path_name::Policy_epoch policy;
    for (const auto& [nonce, value] : bindings) {
        (void)nonce;
        const auto other = std::static_pointer_cast<Binding>(value);
        if (other && other.get() != this && !other->closed && !other->directory.empty() &&
            policy.same(spelling, watch_path(other->directory)))
        {
            throw Observation_failure("subagent_session_already_bound");
        }
    }
}

} // namespace

struct Lifecycle_observer::State
{
    State(std::shared_ptr<observation::Service> service, observation::Configuration configuration)

    :
        driver(std::move(service), configuration)
    {}

    addon::Filesystem_lifecycle_observer driver;
};

Lifecycle_observer::Lifecycle_observer()

:
    Lifecycle_observer(std::make_shared<observation::Service>(), {})
{}

Lifecycle_observer::Lifecycle_observer(
    std::shared_ptr<observation::Service> service,
    observation::Configuration configuration)

:
    m_state(std::make_unique<State>(std::move(service), configuration))
{}

Lifecycle_observer::~Lifecycle_observer() = default;

addon::Classify_event_result Lifecycle_observer::classify(const addon::Classify_event_request& request)
{
    auto result = qwen::classify_event(request);
    const auto session = json::text(request.event.body, "session_id");
    const auto& name = request.event.name;
    auto& driver = m_state->driver;
    auto guard = driver.lock();
    if (result.classification == addon::Classification::TRANSITION && name == "SessionStart") {
        if (!driver.reserved(request.transition_nonce)) {
            throw Observation_failure("correlation_not_reserved");
        }
        const auto directory = child_directory(request.event.body, session);
        if (const auto existing = driver.find(request.transition_nonce)) {
            if (existing->session != session) {
                throw Observation_failure("subagent_session_already_bound");
            }
            return result;
        }
        auto binding = std::make_shared<Binding>();
        binding->session   = session;
        binding->nonce     = request.transition_nonce;
        binding->directory = directory;
        driver.bind(std::move(binding));
        return result;
    }
    const auto& current = request.ledger.current_transition;
    if (!current || request.ledger.current_epoch == 0 || current->provider_session_id != session) {
        return result;
    }
    const auto owner = std::static_pointer_cast<Binding>(driver.find(current->nonce));
    if (!owner || owner->session != session) {
        return result;
    }
    auto& binding = *owner;
    if (binding.closed || binding.retired) {
        return {};
    }
    if (name == "SessionEnd" && result.observations.size() == 1 &&
        result.observations.front().session_transition == addon::Session_transition::ENDED)
    {
        // The root's terminal fact follows already admitted child facts. Its
        // final cursor, rather than a watch timeout, releases this correlation.
        binding.finish(std::move(result.observations.front()));
        result.observations.clear();
        result.classification = addon::Classification::IGNORED;
        return result;
    }
    if (name != "SubagentStart") {
        return result;
    }
    const auto& body = request.event.body;
    if (!json::bounded_string(json::member(body, "agent_id"), 256) ||
        !json::bounded_string(json::member(body, "event_id"), 256) ||
        !json::bounded_string(json::member(body, "agent_type"), 160))
    {
        return result;
    }
    const auto event_id = json::text(body, "event_id");
    if (binding.starts.contains(event_id)) {
        return result;
    }
    if (binding.directory.empty()) {
        return result;
    }
    const auto agent = json::text(body, "agent_id");
    auto previous = binding.children.find(agent);
    if (binding.starts.size() == k_retained_bound ||
        (previous == binding.children.end() && binding.children.size() == k_retained_bound))
    {
        return result;
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
    if (!binding.can_start_child()) {
        return result;
    }
    binding.starts.insert(event_id);
    auto inserted = binding.children.insert_or_assign(agent, std::move(child)).first;
    child_event(binding, inserted->second, true);
    binding.request_rescan();
    return result;
}

void Lifecycle_observer::handle(addon::Invocation_context& context)
{
    if (m_state->driver.handle(context)) {
        return;
    }
    try {
        addon::Contract_failure failure;
        const auto& invocation = context.request();
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
        context.reject_unknown_operation();
    }
    catch (const Observation_failure& exception) {
        context.fail(addon::Result_status::FAILED, exception.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "subagent_observation_failed");
    }
}

} // namespace qwen
