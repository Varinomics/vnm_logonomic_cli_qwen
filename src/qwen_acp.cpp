#include "qwen_acp.h"
#include "qwen_json.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
// Qwen Code 0.24.3 announces its command catalog in one record of about
// 560 KiB in a fresh profile. One record stays within the SDK stream bound.
constexpr std::size_t k_record_limit = 1024 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;
constexpr std::int64_t k_active_work_interval_ms = 5000;
constexpr std::int64_t k_active_work_max_interval_ms = 60000;
constexpr std::int64_t k_active_work_stale_intervals = 3;
constexpr std::int64_t k_close_drain_timeout_ms = 8000;
constexpr std::int64_t k_max_rpc_id = 9007199254740991;
constexpr std::size_t k_active_work_limit = 1024;
constexpr std::array<std::string_view, 6> k_active_work_categories{
    "agent", "notification", "shell", "session", "workflow", "tool"};

std::string rpc(std::int64_t id, const char* method, Json params)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
        {"method", Json(method)}, {"params", std::move(params)}})) + '\n';
}

addon::Structured_inspect_result failure(const char* reason)
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = reason;
    return result;
}

bool optional_object(const Json& value, std::string_view name)
{
    const auto* found = value.find(name);
    return !found || found->is_object();
}

// Approve only the tools injected for this Task. Never select a persistent
// approval or an option that also changes the launcher's permission mode.
std::string permission_reply(const Json& request, const Json& state)
{
    const auto& params = json_util::member(request, "params");
    const auto& id = json_util::member(request, "id");
    const auto& options = json_util::member(params, "options");
    if (!json_util::exact_keys(request, {"jsonrpc", "id", "method", "params"}) || json_util::text(request, "jsonrpc") != "2.0" ||
        !(id.is_integer() || json_util::bounded_string(id, 512)) || json_util::text(params, "sessionId") != json_util::text(state, "session_id") ||
        !json_util::bounded_string(json_util::member(json_util::member(params, "toolCall"), "toolCallId"), 512) || !options.is_array()) return {};
    const auto name = json_util::text(json_util::member(json_util::member(params, "toolCall"), "_meta"), "toolName");
    const auto& tools = json_util::member(state, "task_tools").as_array();
    const bool allowed = std::any_of(tools.begin(), tools.end(), [&](const Json& tool) { return tool.as_string() == name; });
    const std::string chosen = allowed ? "proceed_once" : "cancel";
    int matches = 0;
    for (const auto& option : options.as_array()) {
        if (json_util::text(option, "optionId") != chosen) continue;
        if (json_util::text(option, "kind") != (allowed ? "allow_once" : "reject_once")) return {};
        ++matches;
    }
    if (matches != 1) return {};
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", id},
        {"result", json_util::object({{"outcome", json_util::object({{"outcome", Json("selected")}, {"optionId", Json(chosen)}})}})}})) + '\n';
}

bool nonnegative_integer(const Json& value)
{
    return value.is_number() && value.as_number() >= 0 && value.as_number() <= 9007199254740991.0 &&
        std::floor(value.as_number()) == value.as_number();
}

bool response(const Json& value, std::int64_t expected)
{
    return json_util::exact_keys(value, {"jsonrpc", "id", "result"}) && json_util::text(value, "jsonrpc") == "2.0" &&
        json_util::member(value, "id").is_integer() && json_util::member(value, "id").as_integer() == expected &&
        json_util::member(value, "result").is_object();
}

bool configuration_matches(const Json& value, std::string_view id, std::string_view expected)
{
    const auto& options = json_util::member(value, "configOptions");
    if (!options.is_array()) return false;
    bool matched = false;
    for (const auto& option : options.as_array()) {
        if (!option.is_object()) return false;
        if (json_util::text(option, "id") == id) {
            if (matched || json_util::text(option, "currentValue") != expected) return false;
            matched = true;
        }
    }
    return matched;
}

std::optional<const char*> response_failure(const Json& value, std::int64_t expected)
{
    if (!json_util::exact_keys(value, {"jsonrpc", "id", "error"}) || json_util::text(value, "jsonrpc") != "2.0" ||
        !json_util::member(value, "id").is_integer() || json_util::member(value, "id").as_integer() != expected) return std::nullopt;
    const auto& error = json_util::member(value, "error");
    if (!json_util::only_keys(error, {"code", "message", "data"}) || !json_util::member(error, "code").is_integer() ||
        !json_util::bounded_string(json_util::member(error, "message"), 16 * 1024) || !optional_object(error, "data")) {
        return "invalid_acp_error";
    }
    return json_util::member(error, "code").as_integer() == -32000 ? "authentication_required" : "acp_request_failed";
}

bool valid_initialize(const Json& result)
{
    if (!json_util::only_keys(result, {"protocolVersion", "agentCapabilities", "agentInfo", "authMethods", "_meta"}) ||
        !json_util::member(result, "protocolVersion").is_integer() || json_util::member(result, "protocolVersion").as_integer() != 1 ||
        !json_util::member(result, "agentCapabilities").is_object() || !optional_object(result, "_meta")) return false;
    const auto& info = json_util::member(result, "agentInfo");
    if (!json_util::only_keys(info, {"name", "title", "version"}) || json_util::text(info, "name") != "qwen-code" ||
        !json_util::bounded_string(json_util::member(info, "title"), 512) || !json_util::bounded_string(json_util::member(info, "version"), 512)) return false;
    const auto& methods = json_util::member(result, "authMethods");
    if (!methods.is_array() || methods.as_array().size() > 16) return false;
    std::vector<std::string> method_ids;
    for (const auto& method : methods.as_array()) {
        if (!json_util::only_keys(method, {"id", "name", "description", "_meta"}) ||
            !json_util::bounded_string(json_util::member(method, "id"), 256) ||
            !json_util::bounded_string(json_util::member(method, "name"), 512) ||
            !json_util::bounded_string(json_util::member(method, "description"), 4096) || !optional_object(method, "_meta")) return false;
        const auto id = json_util::text(method, "id");
        if (std::find(method_ids.begin(), method_ids.end(), id) != method_ids.end()) return false;
        method_ids.push_back(id);
    }
    return true;
}

// Native full snapshots cover both workers and the parent turns consuming
// their results. A child reaching a terminal status alone is not quiescence.
struct active_work_state_t
{
    std::int64_t interval_ms       = 0;
    std::int64_t sequence          = 0;
    std::int64_t observed_at_ms    = 0;
    std::int64_t close_request_id  = 5;
    bool         session_observed = false;
    bool         held             = true;
};

Json encode_active_work(const active_work_state_t& work)
{
    return json_util::object({
        {"interval_ms", Json(work.interval_ms)},
        {"sequence", Json(work.sequence)},
        {"observed_at_ms", Json(work.observed_at_ms)},
        {"close_request_id", Json(work.close_request_id)},
        {"session_observed", Json(work.session_observed)},
        {"held", Json(work.held)}});
}

std::optional<active_work_state_t> decode_active_work(const Json& value)
{
    if (!json_util::exact_keys(value, {"interval_ms", "sequence", "observed_at_ms",
            "close_request_id", "session_observed", "held"}))
    {
        return std::nullopt;
    }
    for (const auto key : {"interval_ms", "sequence", "close_request_id"}) {
        if (!json_util::member(value, key).is_integer() ||
            !nonnegative_integer(json_util::member(value, key)))
        {
            return std::nullopt;
        }
    }
    if (!json_util::member(value, "observed_at_ms").is_integer() ||
        !json_util::member(value, "session_observed").is_boolean() ||
        !json_util::member(value, "held").is_boolean())
    {
        return std::nullopt;
    }
    active_work_state_t work;
    work.interval_ms       = json_util::member(value, "interval_ms").as_integer();
    work.sequence          = json_util::member(value, "sequence").as_integer();
    work.observed_at_ms    = json_util::member(value, "observed_at_ms").as_integer();
    work.close_request_id  = json_util::member(value, "close_request_id").as_integer();
    work.session_observed = json_util::member(value, "session_observed").as_boolean();
    work.held             = json_util::member(value, "held").as_boolean();
    if ((work.interval_ms != 0 &&
            (work.interval_ms < k_active_work_interval_ms || work.interval_ms > k_active_work_max_interval_ms)) ||
        work.close_request_id < 5 || work.close_request_id > k_max_rpc_id)
    {
        return std::nullopt;
    }
    return work;
}

std::optional<std::int64_t> active_work_interval(const Json& result)
{
    const auto& capability = json_util::member(
        json_util::member(result, "_meta"), "qwen.daemon.activeWorkHeartbeat");
    const auto& interval   = json_util::member(capability, "intervalMs");
    const auto& categories = json_util::member(capability, "categories");
    if (!capability.is_object() || !json_util::member(capability, "v").is_integer() ||
        json_util::member(capability, "v").as_integer() != 1 || !interval.is_integer() ||
        interval.as_integer() < k_active_work_interval_ms || interval.as_integer() > k_active_work_max_interval_ms ||
        !categories.is_array() || categories.as_array().size() > k_active_work_limit)
    {
        return std::nullopt;
    }
    std::set<std::string> accepted;
    for (const auto& category : categories.as_array()) {
        if (!json_util::bounded_string(category, 128) || !accepted.insert(category.as_string()).second) {
            return std::nullopt;
        }
    }
    for (const auto category : k_active_work_categories) {
        if (!accepted.contains(std::string(category))) {
            return std::nullopt;
        }
    }
    return interval.as_integer();
}

bool valid_active_work_holds(const Json& holds)
{
    if (!holds.is_array() || holds.as_array().size() > k_active_work_limit) {
        return false;
    }
    for (const auto& hold : holds.as_array()) {
        if (!hold.is_object() || !json_util::bounded_string(json_util::member(hold, "category"), 128) ||
            !json_util::bounded_string(json_util::member(hold, "id"), 1024))
        {
            return false;
        }
    }
    return true;
}

bool accept_active_work(
    const Json& value, std::string_view session, std::int64_t now_ms, active_work_state_t& work)
{
    const auto& params   = json_util::member(value, "params");
    const auto& sequence = json_util::member(params, "seq");
    const auto& sessions = json_util::member(params, "sessions");
    if (!json_util::exact_keys(value, {"jsonrpc", "method", "params"}) ||
        json_util::text(value, "jsonrpc") != "2.0" || !params.is_object() ||
        !json_util::member(params, "v").is_integer() || json_util::member(params, "v").as_integer() != 1 ||
        !sequence.is_integer() || !nonnegative_integer(sequence) || sequence.as_integer() == 0 ||
        !sessions.is_array() || sessions.as_array().size() > k_active_work_limit)
    {
        return false;
    }
    std::set<std::string> session_ids;
    bool observed = false;
    bool held     = true;
    for (const auto& record : sessions.as_array()) {
        const auto& id    = json_util::member(record, "sessionId");
        const auto& holds = json_util::member(record, "holds");
        if (!record.is_object() || !json_util::bounded_string(id, 512) ||
            !session_ids.insert(id.as_string()).second || !valid_active_work_holds(holds) ||
            (record.find("hasRunningBackgroundTasks") &&
                !json_util::member(record, "hasRunningBackgroundTasks").is_boolean()) ||
            (record.find("finishedBackgroundTurnId") &&
                !json_util::bounded_string(json_util::member(record, "finishedBackgroundTurnId"), 1024)))
        {
            return false;
        }
        if (id.as_string() == session) {
            observed = true;
            held     = !holds.as_array().empty();
        }
    }
    // Native sequence numbers reject reordered/replayed snapshots; a replay
    // must not renew an old observation's freshness.
    if (sequence.as_integer() > work.sequence) {
        work.sequence          = sequence.as_integer();
        work.observed_at_ms    = now_ms;
        work.session_observed = observed;
        work.held             = held;
    }
    return true;
}

bool active_work_is_fresh(const active_work_state_t& work, std::int64_t now_ms)
{
    return work.session_observed && now_ms >= work.observed_at_ms &&
        static_cast<std::uint64_t>(now_ms) - static_cast<std::uint64_t>(work.observed_at_ms) <
            static_cast<std::uint64_t>(work.interval_ms * k_active_work_stale_intervals);
}

bool valid_models(const Json& result)
{
    const auto& models = json_util::member(result, "models");
    if (!json_util::exact_keys(models, {"currentModelId", "availableModels"}) ||
        !json_util::bounded_string(json_util::member(models, "currentModelId"), 1024)) return false;
    const auto& available = json_util::member(models, "availableModels");
    if (!available.is_array() || available.as_array().empty() || available.as_array().size() > 1024) return false;
    bool current_found = false;
    for (const auto& model : available.as_array()) {
        if (!json_util::only_keys(model, {"modelId", "name", "description", "_meta"}) ||
            !json_util::bounded_string(json_util::member(model, "modelId"), 1024) || !json_util::bounded_string(json_util::member(model, "name"), 4096) ||
            !(json_util::member(model, "description").is_null() || json_util::bounded_string(json_util::member(model, "description"), 16 * 1024, true)) ||
            !optional_object(model, "_meta")) return false;
        current_found = current_found || json_util::text(model, "modelId") == json_util::text(models, "currentModelId");
    }
    return current_found;
}

bool valid_session(const Json& result)
{
    if (!json_util::only_keys(result, {"sessionId", "models", "modes", "configOptions", "_meta"}) ||
        !json_util::bounded_string(json_util::member(result, "sessionId"), 512) || !valid_models(result) ||
        !json_util::member(result, "modes").is_object() || !json_util::member(result, "configOptions").is_array() ||
        json_util::member(result, "configOptions").as_array().size() > 1024 || !optional_object(result, "_meta")) return false;
    return true;
}

bool valid_text_update(const Json& update)
{
    const auto& content = json_util::member(update, "content");
    if (!(json_util::only_keys(update, {"sessionUpdate", "content", "_meta"}) && optional_object(update, "_meta") &&
        json_util::exact_keys(content, {"type", "text"}) && json_util::text(content, "type") == "text" &&
        json_util::bounded_string(json_util::member(content, "text"), k_record_limit, true)))
    {
        return false;
    }
    const auto& meta = json_util::member(update, "_meta");
    if (json_util::text(meta, "source") == "background_notification_response") {
        const auto& task = json_util::member(meta, "backgroundTask");
        if (!task.is_object() || !json_util::member(task, "turnComplete").is_boolean() ||
            (task.find("partial") && !json_util::member(task, "partial").is_boolean()))
        {
            return false;
        }
    }
    return true;
}

std::optional<bool> root_message(const Json& update)
{
    const auto& meta = json_util::member(update, "_meta");
    bool child = false;
    for (const auto key : {"parentToolCallId", "subagentType"}) {
        if (meta.find(key)) {
            if (!json_util::bounded_string(json_util::member(meta, key), 1024)) {
                return std::nullopt;
            }
            child = true;
        }
    }
    if (const auto* agent = meta.find("agent")) {
        const auto& depth = json_util::member(*agent, "depth");
        if (!agent->is_object() || !json_util::bounded_string(json_util::member(*agent, "agentName"), 1024) ||
            !nonnegative_integer(depth) || !json_util::bounded_string(json_util::member(*agent, "kind"), 128))
        {
            return std::nullopt;
        }
        const bool root = json_util::text(*agent, "kind") == "root";
        if (root && (depth.as_number() != 0 || json_util::text(*agent, "agentName") != "/root")) {
            return std::nullopt;
        }
        child = child || !root;
    }
    // These are native task-result displays, not the parent's model synthesis.
    const auto source = json_util::text(meta, "source");
    return !child && source != "background_notification" && source != "background_task_completed";
}

bool active_stage(std::string_view stage)
{
    return stage == "await_prompt" || stage == "await_active_work" || stage == "await_close";
}

bool valid_commands(const Json& update)
{
    const auto& commands = json_util::member(update, "availableCommands");
    if (!json_util::only_keys(update, {"sessionUpdate", "availableCommands", "_meta"}) || !optional_object(update, "_meta") ||
        !commands.is_array() || commands.as_array().size() > 1024) return false;
    for (const auto& command : commands.as_array()) {
        const auto& input = json_util::member(command, "input");
        if (!json_util::only_keys(command, {"name", "description", "input", "_meta"}) ||
            !json_util::bounded_string(json_util::member(command, "name"), 512) || !json_util::bounded_string(json_util::member(command, "description"), 4096) ||
            (!input.is_null() && (!json_util::exact_keys(input, {"hint"}) ||
                !json_util::bounded_string(json_util::member(input, "hint"), 4096, true))) || !optional_object(command, "_meta"))
        {
            return false;
        }
    }
    return true;
}

bool valid_tool(const Json& update)
{
    if (!json_util::only_keys(update, {"sessionUpdate", "toolCallId", "title", "kind", "status", "content", "locations",
            "rawInput", "rawOutput", "_meta"}) || !json_util::bounded_string(json_util::member(update, "toolCallId"), 512) ||
        !optional_object(update, "_meta")) return false;
    if (json_util::text(update, "sessionUpdate") == "tool_call" &&
        (!json_util::bounded_string(json_util::member(update, "title"), 4096) || !json_util::bounded_string(json_util::member(update, "status"), 64))) return false;
    for (const auto key : {"kind", "status"}) {
        if (update.find(key) && !json_util::bounded_string(json_util::member(update, key), 64)) return false;
    }
    if (update.find("title") && !json_util::bounded_string(json_util::member(update, "title"), 4096, true)) return false;
    for (const auto key : {"content", "locations"}) {
        if (update.find(key) && !json_util::member(update, key).is_array()) return false;
    }
    return true;
}

bool valid_plan(const Json& update)
{
    const auto& entries = json_util::member(update, "entries");
    if (!json_util::only_keys(update, {"sessionUpdate", "entries", "_meta"}) || !optional_object(update, "_meta") ||
        !entries.is_array() || entries.as_array().size() > 1024) return false;
    for (const auto& entry : entries.as_array()) {
        const auto priority = json_util::text(entry, "priority");
        const auto status   = json_util::text(entry, "status");
        if (!json_util::exact_keys(entry, {"content", "priority", "status"}) ||
            !json_util::bounded_string(json_util::member(entry, "content"), 16 * 1024) ||
            (priority != "low" && priority != "medium" && priority != "high") ||
            (status != "pending" && status != "in_progress" && status != "completed")) return false;
    }
    return true;
}

bool valid_standard_notification(
    const Json& value, std::string_view stage, std::string& session, std::string& output)
{
    const auto& params = json_util::member(value, "params");
    if (!json_util::exact_keys(value, {"jsonrpc", "method", "params"}) || json_util::text(value, "jsonrpc") != "2.0" ||
        json_util::text(value, "method") != "session/update" || !json_util::only_keys(params, {"sessionId", "update", "_meta"}) ||
        !json_util::bounded_string(json_util::member(params, "sessionId"), 512) || !json_util::member(params, "update").is_object() ||
        !optional_object(params, "_meta")) return false;
    const auto announced_session = json_util::text(params, "sessionId");
    if (session.empty() && stage == "await_new") session = announced_session;
    if (session != announced_session) return false;
    const auto& update = json_util::member(params, "update");
    const auto type = json_util::text(update, "sessionUpdate");
    if (type == "available_commands_update") return stage != "await_initialize" && valid_commands(update);
    if (type == "current_mode_update") {
        return (stage == "await_new" || stage == "await_effort" || active_stage(stage)) &&
            json_util::only_keys(update, {"sessionUpdate", "currentModeId", "_meta"}) && optional_object(update, "_meta") &&
            json_util::bounded_string(json_util::member(update, "currentModeId"), 64);
    }
    if (type == "config_option_update") {
        return stage != "await_initialize" && json_util::only_keys(update, {"sessionUpdate", "configOptions", "_meta"}) &&
            json_util::member(update, "configOptions").is_array() && json_util::member(update, "configOptions").as_array().size() <= 1024 &&
            optional_object(update, "_meta");
    }
    if (type == "agent_message_chunk" || type == "agent_thought_chunk" || type == "user_message_chunk") {
        if (!active_stage(stage) || !valid_text_update(update)) {
            return false;
        }
        if (type == "agent_message_chunk") {
            const auto root = root_message(update);
            if (!root) {
                return false;
            }
            if (*root) {
                output += json_util::member(json_util::member(update, "content"), "text").as_string();
            }
        }
        return output.size() <= k_result_limit;
    }
    if (type == "tool_call" || type == "tool_call_update") return active_stage(stage) && valid_tool(update);
    if (type == "plan") return active_stage(stage) && valid_plan(update);
    if (type == "usage_update") {
        return active_stage(stage) && json_util::only_keys(update, {"sessionUpdate", "used", "size", "_meta"}) &&
            nonnegative_integer(json_util::member(update, "used")) && nonnegative_integer(json_util::member(update, "size")) &&
            json_util::member(update, "size").as_number() > 0 && optional_object(update, "_meta");
    }
    if (type == "session_info_update") {
        return active_stage(stage) && json_util::only_keys(update, {"sessionUpdate", "title", "_meta"}) &&
            (json_util::member(update, "title").is_null() || json_util::bounded_string(json_util::member(update, "title"), 4096, true)) &&
            optional_object(update, "_meta");
    }
    return false;
}

bool valid_private_notification(const Json& value, std::string_view stage, std::string& session)
{
    const auto& params = json_util::member(value, "params");
    if (!json_util::exact_keys(value, {"jsonrpc", "method", "params"}) || json_util::text(value, "jsonrpc") != "2.0" ||
        stage == "await_initialize" || !params.is_object() || !json_util::bounded_string(json_util::member(params, "sessionId"), 512)) return false;
    const auto announced_session = json_util::text(params, "sessionId");
    if (session.empty() && stage == "await_new") session = announced_session;
    if (session != announced_session) return false;
    const auto method = json_util::text(value, "method");
    if (method == "qwen/notify/session/mode-update") {
        return (stage == "await_new" || stage == "await_effort" || active_stage(stage)) &&
            json_util::exact_keys(params, {"v", "sessionId", "currentModeId"}) && json_util::member(params, "v").is_integer() &&
            json_util::member(params, "v").as_integer() == 1 && json_util::bounded_string(json_util::member(params, "currentModeId"), 64);
    }
    const bool setup_notification = method == "qwen/notify/session/mcp-budget-event" ||
        method == "qwen/notify/session/sources-changed";
    if (setup_notification) return stage == "await_new" || stage == "await_effort" || active_stage(stage);
    if (!active_stage(stage)) return false;
    if (method == "qwen/notify/session/last-user-prompt") {
        const auto& timestamp = json_util::member(params, "timestamp");
        return json_util::exact_keys(params, {"v", "sessionId", "timestamp"}) &&
            json_util::member(params, "v").is_integer() && json_util::member(params, "v").as_integer() == 1 &&
            (timestamp.is_null() || json_util::bounded_string(timestamp, 64));
    }
    return method == "qwen/notify/session/prompt-suggestion" ||
        method == "qwen/notify/session/title-update" || method == "qwen/notify/session/pr-binding" ||
        method == "qwen/notify/session/recording-degraded" || method == "qwen/notify/session/model-update" ||
        method == "qwen/notify/session/artifact-event" || method == "qwen/notify/session/terminal-sequence" ||
        method == "qwen/notify/session/generation/event" || method == "_qwencode/start_turn" ||
        method == "_qwencode/end_turn";
}

bool valid_drain_notification(const Json& value, std::string session)
{
    const auto& params = json_util::member(value, "params");
    const auto method = json_util::text(value, "method");
    if (method == "session/update") {
        const auto type = json_util::text(json_util::member(params, "update"), "sessionUpdate");
        const bool advisory = type == "available_commands_update" || type == "current_mode_update" ||
            type == "config_option_update" || type == "session_info_update";
        std::string ignored_output;
        return advisory && valid_standard_notification(value, "await_prompt", session, ignored_output);
    }
    if (!json_util::exact_keys(value, {"jsonrpc", "method", "params"}) || json_util::text(value, "jsonrpc") != "2.0" ||
        !params.is_object() || json_util::text(params, "sessionId") != session)
    {
        return false;
    }
    // SessionSourcesController uses an integer revision without a version
    // field; other native advisory producers attach v:1 explicitly.
    if (method == "qwen/notify/session/sources-changed") {
        return json_util::exact_keys(params, {"sessionId", "revision"}) && nonnegative_integer(json_util::member(params, "revision"));
    }
    if (!json_util::member(params, "v").is_integer() || json_util::member(params, "v").as_integer() != 1) {
        return false;
    }
    if (method == "qwen/notify/session/last-user-prompt") {
        return valid_private_notification(value, "await_prompt", session);
    }
    if (method == "qwen/notify/session/title-update") {
        return json_util::exact_keys(params, {"v", "sessionId", "title", "titleSource"}) &&
            json_util::bounded_string(json_util::member(params, "title"), 4096, true) &&
            (json_util::text(params, "titleSource") == "auto" || json_util::text(params, "titleSource") == "manual");
    }
    if (method == "qwen/notify/session/prompt-suggestion") {
        return json_util::exact_keys(params, {"v", "sessionId", "suggestion", "promptId"}) &&
            json_util::bounded_string(json_util::member(params, "suggestion"), k_record_limit) &&
            json_util::bounded_string(json_util::member(params, "promptId"), 1024);
    }
    if (method == "qwen/notify/session/pr-binding") {
        const auto& pr = json_util::member(params, "pr");
        return json_util::exact_keys(params, {"v", "sessionId", "pr"}) && json_util::exact_keys(pr, {"number", "url"}) &&
            nonnegative_integer(json_util::member(pr, "number")) && json_util::member(pr, "number").as_number() > 0 &&
            json_util::bounded_string(json_util::member(pr, "url"), 16 * 1024);
    }
    if (method == "qwen/notify/session/recording-degraded") {
        return json_util::exact_keys(params, {"v", "sessionId", "reason"}) && json_util::text(params, "reason") == "write_failed";
    }
    if (method == "qwen/notify/session/model-update") {
        return json_util::exact_keys(params, {"v", "sessionId", "currentModelId"}) &&
            json_util::bounded_string(json_util::member(params, "currentModelId"), 1024);
    }
    if (method == "qwen/notify/session/mode-update") {
        return valid_private_notification(value, "await_prompt", session);
    }
    if (method != "qwen/notify/session/mcp-budget-event" ||
        !nonnegative_integer(json_util::member(params, "liveCount")) ||
        !nonnegative_integer(json_util::member(params, "reservedCount")) ||
        !nonnegative_integer(json_util::member(params, "budget")) ||
        (params.find("scope") && json_util::text(params, "scope") != "workspace"))
    {
        return false;
    }
    if (json_util::text(params, "kind") == "budget_warning") {
        return json_util::only_keys(params, {"v", "sessionId", "scope", "kind", "liveCount", "reservedCount",
                "budget", "thresholdRatio", "mode"}) &&
            json_util::member(params, "thresholdRatio").is_number() &&
            json_util::member(params, "thresholdRatio").as_number() > 0 &&
            json_util::member(params, "thresholdRatio").as_number() <= 1 &&
            (json_util::text(params, "mode") == "warn" || json_util::text(params, "mode") == "enforce");
    }
    if (json_util::text(params, "kind") != "refused_batch" || json_util::text(params, "mode") != "enforce" ||
        !json_util::only_keys(params, {"v", "sessionId", "scope", "kind", "liveCount", "reservedCount",
            "budget", "refusedServers", "mode"}) || !json_util::member(params, "refusedServers").is_array())
    {
        return false;
    }
    for (const auto& server : json_util::member(params, "refusedServers").as_array()) {
        if (!json_util::exact_keys(server, {"name", "transport", "reason"}) ||
            !json_util::bounded_string(json_util::member(server, "name"), 1024) ||
            !json_util::bounded_string(json_util::member(server, "transport"), 64) || json_util::text(server, "reason") != "budget_exhausted")
        {
            return false;
        }
    }
    return true;
}

// Qwen asks its ACP client between tool batches for user messages queued
// during the turn. This client queues none; Qwen documents "method not found"
// as the answer of a client without that queue and then stops asking.
bool mid_turn_queue_request(const Json& value, std::string_view session)
{
    const auto& id     = json_util::member(value, "id");
    const auto& params = json_util::member(value, "params");
    return json_util::exact_keys(value, {"jsonrpc", "id", "method", "params"}) && json_util::text(value, "jsonrpc") == "2.0" &&
        json_util::text(value, "method") == "craft/drainMidTurnQueue" && (id.is_integer() || json_util::bounded_string(id, 512)) &&
        json_util::only_keys(params, {"sessionId", "promptId", "todoStopGuardWatchQueuedPrompt"}) &&
        !session.empty() && json_util::text(params, "sessionId") == session;
}

bool background_turn_params(const Json& params, std::string_view session)
{
    if (!params.is_object() || session.empty() || json_util::text(params, "sessionId") != session ||
        json_util::text(params, "source") != "background_notification" ||
        !json_util::bounded_string(json_util::member(params, "turnId"), 1024) ||
        !json_util::bounded_string(json_util::member(params, "taskId"), 1024) ||
        !json_util::bounded_string(json_util::member(params, "kind"), 128) ||
        !nonnegative_integer(json_util::member(params, "startedAt")))
    {
        return false;
    }
    for (const auto key : {"toolUseId", "sourceTurnId", "afterPromptId", "label"}) {
        if (params.find(key) && !json_util::bounded_string(json_util::member(params, key), 4096)) {
            return false;
        }
    }
    return true;
}

bool background_turn_request(const Json& value, std::string_view session)
{
    const auto& id = json_util::member(value, "id");
    return json_util::exact_keys(value, {"jsonrpc", "id", "method", "params"}) &&
        json_util::text(value, "jsonrpc") == "2.0" &&
        json_util::text(value, "method") == "_qwencode/start_turn" &&
        (id.is_integer() || json_util::bounded_string(id, 512)) &&
        background_turn_params(json_util::member(value, "params"), session);
}

std::optional<const char*> observe_background_turn(const Json& value, Json& state)
{
    const auto method = json_util::text(value, "method");
    const auto& params = json_util::member(value, "params");
    if (json_util::text(params, "source") == "background_notification") {
        if (method == "_qwencode/start_turn") {
            if (!background_turn_params(params, json_util::text(state, "session_id")) ||
                !json_util::text(json_util::member(state, "background_turn"), "turnId").empty())
            {
                return "invalid_acp_background_turn";
            }
            state.set("background_turn", params);
            state.set("background_turn_complete", Json(false));
        }
        else
        if (method == "_qwencode/end_turn") {
            if (!json_util::bounded_string(json_util::member(params, "turnId"), 1024) ||
                json_util::text(params, "turnId") != json_util::text(json_util::member(state, "background_turn"), "turnId"))
            {
                return "invalid_acp_background_turn";
            }
            const auto reason = json_util::text(params, "reason");
            if (reason == "cancelled") {
                return "acp_cancelled";
            }
            if (reason != "end_turn") {
                return "acp_background_incomplete";
            }
            // Native errors before the first response segment can still end
            // with reason=end_turn. Require the typed synthesis completion,
            // never an error's human-readable text or the first parent reply.
            if (!json_util::member(state, "background_turn_complete").as_boolean()) {
                return "acp_background_incomplete";
            }
            state.set("background_turn", Json::object());
            state.set("background_turn_complete", Json(false));
        }
    }
    const auto& update = json_util::member(params, "update");
    const auto& meta   = json_util::member(update, "_meta");
    if (method == "session/update" && json_util::text(update, "sessionUpdate") == "agent_message_chunk" &&
        json_util::text(meta, "source") == "background_notification_response")
    {
        const auto& task = json_util::member(meta, "backgroundTask");
        if (json_util::text(task, "turnId").empty() ||
            json_util::text(task, "turnId") != json_util::text(json_util::member(state, "background_turn"), "turnId"))
        {
            return "invalid_acp_background_turn";
        }
        if (const auto* partial = task.find("partial"); partial && partial->as_boolean()) {
            return "acp_background_incomplete";
        }
        state.set("background_turn_complete", json_util::member(task, "turnComplete"));
    }
    return std::nullopt;
}

bool work_notification(const Json& value)
{
    const auto method = json_util::text(value, "method");
    if (method == "_qwencode/start_turn" || method == "_qwencode/end_turn" ||
        method == "qwen/notify/session/generation/event")
    {
        return true;
    }
    if (method != "session/update") {
        return false;
    }
    const auto type = json_util::text(json_util::member(json_util::member(value, "params"), "update"), "sessionUpdate");
    return type == "agent_message_chunk" || type == "agent_thought_chunk" || type == "user_message_chunk" ||
        type == "tool_call" || type == "tool_call_update" || type == "plan" || type == "usage_update";
}

std::string method_not_found(const Json& id)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", id},
        {"error", json_util::object({{"code", Json(-32601)}, {"message", Json("Method not found")}})}})) + '\n';
}

std::string initial_acp_input(const addon::Session_launch_context& context, Json servers)
{
    auto categories = Json::array();
    for (const auto category : k_active_work_categories) {
        categories.push_back(Json(std::string(category)));
    }
    auto capability = json_util::object({{"v", Json(1)}, {"intervalMs", Json(k_active_work_interval_ms)},
        {"categories", std::move(categories)}});
    return rpc(1, "initialize", json_util::object({
        {"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
        {"_meta", json_util::object({{"qwen.daemon.activeWorkHeartbeat", std::move(capability)}})}})) +
        rpc(2, "session/new", json_util::object({{"cwd", Json(context.working_directory)}, {"mcpServers", std::move(servers)}}));
}

bool valid_receipts(const Json& receipts)
{
    if (!receipts.is_array()) return false;
    return std::all_of(receipts.as_array().begin(), receipts.as_array().end(), [](const Json& receipt) {
        const auto nonce = json_util::text(receipt, "nonce");
        return json_util::exact_keys(receipt, {"path", "nonce"}) && json_util::bounded_string(json_util::member(receipt, "path"), 32 * 1024) &&
            nonce.size() == 32 && std::all_of(nonce.begin(), nonce.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
    });
}

// The relay creates each receipt empty when its connection starts and writes
// the nonce only after the Task server's catalog succeeded.
bool receipts_hold_nonces(const Json& receipts)
{
    namespace fs = std::filesystem;
    return std::all_of(receipts.as_array().begin(), receipts.as_array().end(), [](const Json& receipt) {
        const auto path = addon::path_from_utf8(json_util::text(receipt, "path"));
        std::error_code error;
        if (!fs::is_regular_file(fs::symlink_status(path, error)) || error) return false;
        std::ifstream file(path, std::ios::binary);
        char bytes[33]{};
        file.read(bytes, sizeof(bytes));
        return file.gcount() == 32 && std::string_view(bytes, 32) == json_util::text(receipt, "nonce");
    });
}

// Qwen's normalizeToolNameForProvider uses unsigned FNV-1a and a seven
// digit base-36 suffix. Task/server/action identifiers are ASCII SDK tokens.
std::string task_tool_name(std::string name)
{
    auto normalized = name;
    for (auto& c : normalized) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-')) c = '_';
    }
    if (name.size() <= 63 && normalized == name) return name;
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : name) { hash ^= c; hash *= 16777619u; }
    std::string suffix(7, '0');
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) {
        *it = "0123456789abcdefghijklmnopqrstuvwxyz"[hash % 36];
        hash /= 36;
    }
    return normalized.substr(0, 55) + "_" + suffix;
}

} // namespace

std::vector<Task_server_receipt> task_server_receipts(const addon::Delegated_prepare_request& request)
{
    std::vector<Task_server_receipt> receipts;
    for (const auto& server : request.control.servers) {
        const auto nonce = addon::random_nonce_hex();
        const auto stem = "mcp/server-" + nonce;
        // The host accepts the receipt path only in its clean '/'-separated
        // form; a native join would insert '\' on Windows.
        const auto path = (addon::path_from_utf8(request.control.private_artifact_root) / (stem + ".ready")).generic_u8string();
        std::vector<std::string> tools;
        for (const auto& action : server.actions) {
            tools.push_back(task_tool_name("mcp__logonomic-task-" + request.control.task_id + "-" + server.server_id + "__" + action));
        }
        receipts.push_back({server.server_id, std::string(path.begin(), path.end()), nonce, stem + ".connection", std::move(tools)});
    }
    return receipts;
}

std::string initial_acp_input(const addon::Structured_prepare_request& request)
{
    return initial_acp_input(request.context, Json::array());
}

std::string initial_acp_input(const addon::Delegated_prepare_request& request, const std::vector<Task_server_receipt>& receipts)
{
    const auto& connection = request.control.connection.value();
    auto servers = Json::array();
    for (const auto& receipt : receipts) {
        auto arguments = Json::array();
        for (const auto& argument : {std::string("--descriptor"), connection.descriptor_path,
                 std::string("--server"), receipt.server_id, std::string("--attestation-path"), receipt.path,
                 std::string("--attestation-nonce"), receipt.nonce})
        {
            arguments.push_back(Json(argument));
        }
        servers.push_back(json_util::object({{"name", Json("logonomic-task-" + request.control.task_id + "-" + receipt.server_id)},
            {"command", Json(connection.relay_program)}, {"args", std::move(arguments)}, {"env", Json::array()}}));
    }
    return initial_acp_input(request.context, std::move(servers));
}

std::string initial_acp_state(std::string_view prompt, const std::vector<Task_server_receipt>& receipts,
    const addon::Delegated_model_selection* selection)
{
    auto expected = Json::array();
    auto tools = Json::array();
    for (const auto& receipt : receipts) {
        for (const auto& tool : receipt.tool_names) tools.push_back(Json(tool));
        expected.push_back(json_util::object({{"path", Json(receipt.path)}, {"nonce", Json(receipt.nonce)}}));
    }
    auto state = json_util::object({{"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")},
        {"result_base64", Json("")}, {"receipts", std::move(expected)}, {"task_tools", std::move(tools)},
        {"active_work", encode_active_work({})}, {"background_turn", Json::object()},
        {"background_turn_complete", Json(false)}});
    if (selection) {
        state.set("model_selection", addon::encode_delegated_model_selection(*selection));
        state.set("native_model", Json(""));
    }
    return addon::serialize_json(state);
}

addon::Structured_inspect_result inspect_acp(
    const addon::Structured_inspection& inspection, std::chrono::steady_clock::time_point now)
{
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
    if (!parsed || !json_util::only_keys(*parsed, {"stage", "pending_base64", "deferred_input_base64", "prompt_base64", "session_id",
            "result_base64", "receipts", "task_tools", "model_selection", "native_model", "active_work", "background_turn",
            "background_turn_complete"}) ||
        !json_util::member(*parsed, "stage").is_string() || !json_util::bounded_string(json_util::member(*parsed, "session_id"), 512, true) ||
        !json_util::member(*parsed, "background_turn").is_object() ||
        !json_util::member(*parsed, "background_turn_complete").is_boolean() ||
        !valid_receipts(json_util::member(*parsed, "receipts"))) {
        return failure("malformed_acp_state");
    }
    const auto& saved_turn = json_util::member(*parsed, "background_turn");
    if (saved_turn.as_object().empty()
            ? json_util::member(*parsed, "background_turn_complete").as_boolean()
            : !background_turn_params(saved_turn, json_util::text(*parsed, "session_id")))
    {
        return failure("malformed_acp_state");
    }
    auto active_work = decode_active_work(json_util::member(*parsed, "active_work"));
    if (!active_work) {
        return failure("malformed_acp_state");
    }
    if (!json_util::member(*parsed, "task_tools").is_array()) return failure("malformed_acp_state");
    for (const auto& tool : json_util::member(*parsed, "task_tools").as_array()) {
        if (!json_util::bounded_string(tool, 64)) return failure("malformed_acp_state");
    }
    addon::Delegated_model_selection selection;
    if (const auto* record = parsed->find("model_selection")) {
        addon::Contract_failure error;
        const auto decoded = addon::decode_delegated_model_selection(*record, &error);
        if (!decoded || !json_util::bounded_string(json_util::member(*parsed, "native_model"), 1024, true)) {
            return failure("malformed_acp_state");
        }
        selection = *decoded;
    }
    else if (parsed->find("native_model")) return failure("malformed_acp_state");
    const int prompt_id = selection.reasoning_effort ? 5 : 4;
    auto state = std::move(*parsed);
    const auto initial_stage = json_util::text(state, "stage");
    if (initial_stage != "await_initialize" && initial_stage != "await_new" &&
        !active_stage(initial_stage) && initial_stage != "await_effort" &&
        initial_stage != "await_drain") return failure("malformed_acp_state");
    if ((initial_stage != "await_initialize" && active_work->interval_ms == 0) ||
        (initial_stage == "await_close" && active_work->close_request_id == 5))
    {
        return failure("malformed_acp_state");
    }
    for (const auto key : {"pending_base64", "deferred_input_base64", "prompt_base64", "result_base64"}) {
        if (!json_util::member(state, key).is_string()) return failure("malformed_acp_state");
    }
    auto pending = addon::decode_base64(json_util::member(state, "pending_base64").as_string());
    auto deferred_input = addon::decode_base64(json_util::member(state, "deferred_input_base64").as_string());
    auto prompt  = addon::decode_base64(json_util::member(state, "prompt_base64").as_string());
    auto output  = addon::decode_base64(json_util::member(state, "result_base64").as_string());
    if (!pending || !deferred_input || !prompt || !output || output->size() > k_result_limit) return failure("malformed_acp_state");
    const auto state_session = json_util::text(state, "session_id");
    if ((initial_stage == "await_initialize" && (!state_session.empty() || !output->empty())) ||
        (initial_stage == "await_new" && !output->empty()) ||
        (initial_stage == "await_effort" && (!selection.reasoning_effort || state_session.empty() || !output->empty())) ||
        ((active_stage(initial_stage) || initial_stage == "await_drain") &&
            (state_session.empty() || !prompt->empty())) ||
        (initial_stage == "await_drain" && output->empty())) {
        return failure("malformed_acp_state");
    }
    const auto release_prompt = [&]() {
        state.set("stage", Json("await_prompt"));
        state.set("prompt_base64", Json(""));
        auto content = Json::array();
        content.push_back(json_util::object({{"type", Json("text")}, {"text", Json(*prompt)}}));
        return rpc(prompt_id, "session/prompt", json_util::object({{"sessionId", json_util::member(state, "session_id")},
            {"prompt", std::move(content)}}));
    };
    *pending += inspection.stdout_bytes;
    // A reply proposed while a native record is incomplete has not been
    // written yet. Retain that causal boundary across inspection calls.
    std::string input = std::move(*deferred_input);
    // Answers to fully received native requests; unlike our own requests they
    // are written even while a later record is still partial.
    std::string replies;
    bool completed = initial_stage == "await_drain";
    std::string_view remaining = *pending;
    for (;;) {
        const auto end = remaining.find('\n');
        if (end == std::string_view::npos) {
            break;
        }
        if (end == 0 || end > k_record_limit) {
            return failure("invalid_acp_record");
        }
        const auto value = json_util::parse(remaining.substr(0, end));
        remaining.remove_prefix(end + 1);
        if (!value || !value->is_object()) return failure("invalid_acp_record");
        auto stage   = json_util::text(state, "stage");
        auto session = json_util::text(state, "session_id");
        if (json_util::text(*value, "method") == "qwen/notify/channel/active-work") {
            // The native reporter starts during initialize and can outlive a
            // successful session close until this client supplies EOF.
            if (!accept_active_work(*value, session, now_ms, *active_work)) {
                return failure("invalid_acp_active_work");
            }
            continue;
        }
        if (selection.model && json_util::text(*value, "method") == "qwen/notify/session/model-update" &&
            json_util::text(json_util::member(*value, "params"), "currentModelId") != json_util::text(state, "native_model"))
        {
            return failure("model_selection_unavailable");
        }
        if (completed) {
            if (!valid_drain_notification(*value, session)) {
                return failure("out_of_order_acp_response");
            }
            continue;
        }
        if (value->find("method")) {
            if (value->find("id")) {
                if (!input.empty() || !active_stage(stage)) {
                    return failure("unsupported_acp_reverse_request");
                }
                if (json_util::text(*value, "method") == "session/request_permission") {
                    const auto reply = permission_reply(*value, state);
                    if (reply.empty()) return failure("task_permission_response_unavailable");
                    replies += reply;
                }
                else if (mid_turn_queue_request(*value, session)) replies += method_not_found(json_util::member(*value, "id"));
                else
                if (background_turn_request(*value, session)) {
                    const auto& params = json_util::member(*value, "params");
                    const auto& admitted = json_util::member(state, "background_turn");
                    if (!admitted.as_object().empty() &&
                        addon::serialize_json_canonical(params) != addon::serialize_json_canonical(admitted))
                    {
                        return failure("unsupported_acp_reverse_request");
                    }
                    if (admitted.as_object().empty()) {
                        state.set("background_turn", params);
                        state.set("background_turn_complete", Json(false));
                    }
                    // Qwen retries the same retained turn when admission times
                    // out. Reply to the current RPC id without resetting any
                    // completion evidence. -32601 is its documented response
                    // for clients without a host-owned admission policy.
                    replies += method_not_found(json_util::member(*value, "id"));
                }
                else {
                    return failure("unsupported_acp_reverse_request");
                }
                active_work->session_observed = false;
                continue;
            }
            if (!input.empty()) {
                const auto update_type = json_util::text(json_util::member(json_util::member(*value, "params"), "update"), "sessionUpdate");
                const bool housekeeping = update_type == "available_commands_update" ||
                    update_type == "current_mode_update" || update_type == "config_option_update";
                const auto method = json_util::text(*value, "method");
                const bool setup_private = method == "qwen/notify/session/mode-update" ||
                    method == "qwen/notify/session/mcp-budget-event" || method == "qwen/notify/session/sources-changed";
                if (!housekeeping && !setup_private) {
                    return failure("out_of_order_acp_notification");
                }
            }
            if (!valid_standard_notification(*value, stage, session, *output) &&
                !valid_private_notification(*value, stage, session)) return failure("unauthenticated_acp_notification");
            state.set("session_id", Json(session));
            if (const auto reason = observe_background_turn(*value, state)) {
                return failure(*reason);
            }
            if (work_notification(*value)) {
                active_work->session_observed = false;
            }
            continue;
        }
        if (stage == "await_active_work") {
            return failure("out_of_order_acp_response");
        }
        const std::int64_t expected = stage == "await_initialize" ? 1 : stage == "await_new" ? 2 :
            stage == "await_effort" ? 3 : stage == "await_close" ? active_work->close_request_id : prompt_id;
        if (const auto reason = response_failure(*value, expected)) {
            return failure(stage == "await_effort" && std::string_view(*reason) == "acp_request_failed"
                ? "reasoning_effort_unavailable" : *reason);
        }
        if (!input.empty()) return failure("out_of_order_acp_response");
        const auto& result = json_util::member(*value, "result");
        if (stage == "await_initialize") {
            if (!response(*value, 1) || !valid_initialize(result)) return failure("invalid_acp_initialize");
            const auto interval = active_work_interval(result);
            if (!interval) {
                return failure("acp_active_work_unavailable");
            }
            active_work->interval_ms = *interval;
            state.set("stage", Json("await_new"));
        }
        else if (stage == "await_new") {
            if (!response(*value, 2) || !valid_session(result) ||
                (!session.empty() && session != json_util::text(result, "sessionId"))) return failure("invalid_acp_session");
            // Preparation makes Qwen discover MCP servers before this reply, so
            // a Task server without its catalog receipt did not connect.
            if (!receipts_hold_nonces(json_util::member(state, "receipts"))) return failure("task_servers_not_ready");
            state.set("session_id", json_util::member(result, "sessionId"));
            if (prompt->empty() || !addon::is_valid_utf8(*prompt)) return failure("invalid_acp_prompt");
            if (selection.model || selection.reasoning_effort) {
                state.set("native_model", json_util::member(json_util::member(result, "models"), "currentModelId"));
            }
            if (selection.reasoning_effort) {
                // Qwen persists effort only with an explicit persistence metadata flag.
                state.set("stage", Json("await_effort"));
                input = rpc(3, "session/set_config_option", json_util::object({{"sessionId", json_util::member(state, "session_id")},
                    {"configId", Json("reasoning_effort")}, {"value", Json(*selection.reasoning_effort)}}));
            }
            else input = release_prompt();
        }
        else if (stage == "await_effort") {
            if (!response(*value, 3) || !configuration_matches(result, "reasoning_effort", *selection.reasoning_effort)) {
                return failure("reasoning_effort_unavailable");
            }
            input = release_prompt();
        }
        else if (stage == "await_prompt") {
            // Qwen Code 0.24 adds `_meta`, its checkpoint branch point, to the reply.
            if (!response(*value, prompt_id) || !json_util::only_keys(result, {"stopReason", "_meta"}) || !optional_object(result, "_meta")) {
                return failure("invalid_acp_completion");
            }
            if (json_util::text(result, "stopReason") == "cancelled") return failure("acp_cancelled");
            if (json_util::text(result, "stopReason") != "end_turn") return failure("invalid_acp_completion");
            state.set("stage", Json("await_active_work"));
        }
        else
        if (stage == "await_close") {
            if (!response(*value, expected) || !json_util::only_keys(result, {"sessionId", "closed", "holds"}) ||
                json_util::text(result, "sessionId") != session || !json_util::member(result, "closed").is_boolean())
            {
                return failure("invalid_acp_close");
            }
            if (json_util::member(result, "closed").as_boolean()) {
                if (result.find("holds") || !json_util::text(json_util::member(state, "background_turn"), "turnId").empty()) {
                    return failure("invalid_acp_close");
                }
                if (output->empty()) {
                    return failure("invalid_acp_completion");
                }
                completed = true;
                state.set("stage", Json("await_drain"));
            }
            else {
                const auto& holds = json_util::member(result, "holds");
                if (!valid_active_work_holds(holds) || holds.as_array().empty()) {
                    return failure("invalid_acp_close");
                }
                // A refusal supersedes every snapshot already received; wait
                // for newer evidence before proposing another close.
                active_work->session_observed = false;
                state.set("stage", Json("await_active_work"));
            }
        }
    }
    if (remaining.size() > k_record_limit) return failure("acp_record_limit");
    if (completed && inspection.final) {
        // The native conditional close settled owned work. Clean process exit
        // still guards buffered output and the asynchronous advisory tail.
        if (inspection.timed_out || inspection.exit_code != 0) {
            return failure(inspection.timed_out ? "acp_timeout" : "acp_process_failed");
        }
        if (!remaining.empty()) {
            return failure("acp_incomplete");
        }
        addon::Structured_inspect_result result;
        result.complete     = true;
        result.result_bytes = std::move(*output);
        return result;
    }
    if (inspection.final) return failure(inspection.timed_out ? "acp_timeout" : "acp_incomplete");
    if (json_util::text(state, "stage") == "await_active_work" && remaining.empty() &&
        json_util::text(json_util::member(state, "background_turn"), "turnId").empty() &&
        active_work_is_fresh(*active_work, now_ms) && !active_work->held)
    {
        if (active_work->close_request_id == k_max_rpc_id) {
            return failure("acp_request_id_limit");
        }
        ++active_work->close_request_id;
        state.set("stage", Json("await_close"));
        input = rpc(active_work->close_request_id, "qwen/control/session/close", json_util::object({
            {"sessionId", json_util::member(state, "session_id")}, {"onlyIfUnheld", Json(true)},
            {"requireFlush", Json(true)}, {"drainTimeoutMs", Json(k_close_drain_timeout_ms)}}));
    }
    // Never release a native request over an already observed, unclassified
    // output prefix. Complete housekeeping may be accepted, but a task reply
    // produced before its request must still be rejected on the next call.
    const bool defer_input = !remaining.empty() && !input.empty();
    state.set("deferred_input_base64", Json(defer_input ? addon::encode_base64(input) : ""));
    state.set("pending_base64", Json(addon::encode_base64(remaining)));
    state.set("result_base64", Json(addon::encode_base64(*output)));
    state.set("active_work", encode_active_work(*active_work));
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(state);
    result.close_stdin = completed && initial_stage != "await_drain";
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!defer_input) replies += input;
    if (!replies.empty()) result.stdin_bytes = std::move(replies);
    return result;
}

} // namespace qwen
