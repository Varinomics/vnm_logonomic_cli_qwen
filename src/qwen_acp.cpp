#include "qwen_acp.h"
#include "qwen_json.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

std::string rpc(int id, const char* method, Json params)
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

bool response(const Json& value, int expected)
{
    return json_util::exact_keys(value, {"jsonrpc", "id", "result"}) && json_util::text(value, "jsonrpc") == "2.0" &&
        json_util::member(value, "id").is_integer() && json_util::member(value, "id").as_integer() == expected &&
        json_util::member(value, "result").is_object();
}

std::optional<const char*> response_failure(const Json& value, int expected)
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
    return json_util::only_keys(update, {"sessionUpdate", "content", "_meta"}) && optional_object(update, "_meta") &&
        json_util::exact_keys(content, {"type", "text"}) && json_util::text(content, "type") == "text" &&
        json_util::bounded_string(json_util::member(content, "text"), k_record_limit, true);
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
        return (stage == "await_new" || stage == "await_prompt") &&
            json_util::only_keys(update, {"sessionUpdate", "currentModeId", "_meta"}) && optional_object(update, "_meta") &&
            json_util::bounded_string(json_util::member(update, "currentModeId"), 64);
    }
    if (type == "config_option_update") {
        return stage != "await_initialize" && json_util::only_keys(update, {"sessionUpdate", "configOptions", "_meta"}) &&
            json_util::member(update, "configOptions").is_array() && json_util::member(update, "configOptions").as_array().size() <= 1024 &&
            optional_object(update, "_meta");
    }
    if (type == "agent_message_chunk" || type == "agent_thought_chunk" || type == "user_message_chunk") {
        if (stage != "await_prompt" || !valid_text_update(update)) return false;
        if (type == "agent_message_chunk") output += json_util::text(json_util::member(update, "content"), "text");
        return output.size() <= k_result_limit;
    }
    if (type == "tool_call" || type == "tool_call_update") return stage == "await_prompt" && valid_tool(update);
    if (type == "plan") return stage == "await_prompt" && valid_plan(update);
    if (type == "usage_update") {
        return stage == "await_prompt" && json_util::only_keys(update, {"sessionUpdate", "used", "size", "_meta"}) &&
            nonnegative_integer(json_util::member(update, "used")) && nonnegative_integer(json_util::member(update, "size")) &&
            json_util::member(update, "size").as_number() > 0 && optional_object(update, "_meta");
    }
    if (type == "session_info_update") {
        return stage == "await_prompt" && json_util::only_keys(update, {"sessionUpdate", "title", "_meta"}) &&
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
        return (stage == "await_new" || stage == "await_prompt") &&
            json_util::exact_keys(params, {"v", "sessionId", "currentModeId"}) && json_util::member(params, "v").is_integer() &&
            json_util::member(params, "v").as_integer() == 1 && json_util::bounded_string(json_util::member(params, "currentModeId"), 64);
    }
    const bool setup_notification = method == "qwen/notify/session/mcp-budget-event" ||
        method == "qwen/notify/session/sources-changed";
    if (setup_notification) return stage == "await_new" || stage == "await_prompt";
    if (stage != "await_prompt") return false;
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

std::string method_not_found(const Json& id)
{
    return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", id},
        {"error", json_util::object({{"code", Json(-32601)}, {"message", Json("Method not found")}})}})) + '\n';
}

std::string initial_acp_input(const addon::Session_launch_context& context, Json servers)
{
    return rpc(1, "initialize", json_util::object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()}})) +
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

std::string initial_acp_state(std::string_view prompt, const std::vector<Task_server_receipt>& receipts)
{
    auto expected = Json::array();
    auto tools = Json::array();
    for (const auto& receipt : receipts) {
        for (const auto& tool : receipt.tool_names) tools.push_back(Json(tool));
        expected.push_back(json_util::object({{"path", Json(receipt.path)}, {"nonce", Json(receipt.nonce)}}));
    }
    return addon::serialize_json(json_util::object({{"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")},
        {"result_base64", Json("")}, {"receipts", std::move(expected)}, {"task_tools", std::move(tools)}}));
}

addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    const auto parsed = addon::parse_exchange_protocol_state(inspection.protocol_state);
    if (!parsed || !json_util::exact_keys(*parsed, {"stage", "pending_base64", "deferred_input_base64", "prompt_base64", "session_id",
            "result_base64", "receipts", "task_tools"}) ||
        !json_util::member(*parsed, "stage").is_string() || !json_util::bounded_string(json_util::member(*parsed, "session_id"), 512, true) ||
        !valid_receipts(json_util::member(*parsed, "receipts"))) {
        return failure("malformed_acp_state");
    }
    if (!json_util::member(*parsed, "task_tools").is_array()) return failure("malformed_acp_state");
    for (const auto& tool : json_util::member(*parsed, "task_tools").as_array()) {
        if (!json_util::bounded_string(tool, 64)) return failure("malformed_acp_state");
    }
    auto state = *parsed;
    const auto initial_stage = json_util::text(state, "stage");
    if (initial_stage != "await_initialize" && initial_stage != "await_new" &&
        initial_stage != "await_prompt" &&
        initial_stage != "await_drain") return failure("malformed_acp_state");
    for (const auto key : {"pending_base64", "deferred_input_base64", "prompt_base64", "result_base64"}) {
        if (!json_util::member(state, key).is_string()) return failure("malformed_acp_state");
    }
    auto pending = addon::decode_base64(json_util::text(state, "pending_base64"));
    auto deferred_input = addon::decode_base64(json_util::text(state, "deferred_input_base64"));
    auto prompt  = addon::decode_base64(json_util::text(state, "prompt_base64"));
    auto output  = addon::decode_base64(json_util::text(state, "result_base64"));
    if (!pending || !deferred_input || !prompt || !output || output->size() > k_result_limit) return failure("malformed_acp_state");
    const auto state_session = json_util::text(state, "session_id");
    if ((initial_stage == "await_initialize" && (!state_session.empty() || !output->empty())) ||
        (initial_stage == "await_new" && !output->empty()) ||
        ((initial_stage == "await_prompt" || initial_stage == "await_drain") &&
            (state_session.empty() || !prompt->empty())) ||
        (initial_stage == "await_drain" && output->empty())) {
        return failure("malformed_acp_state");
    }
    *pending += inspection.stdout_bytes;
    // A reply proposed while a native record is incomplete has not been
    // written yet. Retain that causal boundary across inspection calls.
    std::string input = std::move(*deferred_input);
    // Answers to fully received native requests; unlike our own requests they
    // are written even while a later record is still partial.
    std::string replies;
    bool completed = initial_stage == "await_drain";
    for (;;) {
        const auto end = pending->find('\n');
        if (end == std::string::npos) break;
        if (end == 0 || end > k_record_limit) return failure("invalid_acp_record");
        const auto value = json_util::parse(std::string_view(*pending).substr(0, end));
        pending->erase(0, end + 1);
        if (!value || !value->is_object()) return failure("invalid_acp_record");
        auto stage   = json_util::text(state, "stage");
        auto session = json_util::text(state, "session_id");
        if (completed) {
            if (!valid_drain_notification(*value, session)) {
                return failure("out_of_order_acp_response");
            }
            continue;
        }
        if (value->find("method")) {
            if (value->find("id")) {
                if (!input.empty() || stage != "await_prompt") return failure("unsupported_acp_reverse_request");
                if (json_util::text(*value, "method") == "session/request_permission") {
                    const auto reply = permission_reply(*value, state);
                    if (reply.empty()) return failure("task_permission_response_unavailable");
                    replies += reply;
                }
                else if (mid_turn_queue_request(*value, session)) replies += method_not_found(json_util::member(*value, "id"));
                else return failure("unsupported_acp_reverse_request");
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
            continue;
        }
        const int expected = stage == "await_initialize" ? 1 : stage == "await_new" ? 2 : 4;
        if (const auto reason = response_failure(*value, expected)) return failure(*reason);
        if (!input.empty()) return failure("out_of_order_acp_response");
        const auto& result = json_util::member(*value, "result");
        if (stage == "await_initialize") {
            if (!response(*value, 1) || !valid_initialize(result)) return failure("invalid_acp_initialize");
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
            state.set("stage", Json("await_prompt"));
            state.set("prompt_base64", Json(""));
            auto content = Json::array();
            content.push_back(json_util::object({{"type", Json("text")}, {"text", Json(*prompt)}}));
            input = rpc(4, "session/prompt", json_util::object({{"sessionId", json_util::member(state, "session_id")},
                {"prompt", std::move(content)}}));
        }
        else if (stage == "await_prompt") {
            // Qwen Code 0.24 adds `_meta`, its checkpoint branch point, to the reply.
            if (!response(*value, 4) || !json_util::only_keys(result, {"stopReason", "_meta"}) || !optional_object(result, "_meta")) {
                return failure("invalid_acp_completion");
            }
            if (json_util::text(result, "stopReason") == "cancelled") return failure("acp_cancelled");
            if (json_util::text(result, "stopReason") != "end_turn" || output->empty()) return failure("invalid_acp_completion");
            completed = true;
            state.set("stage", Json("await_drain"));
        }
    }
    if (pending->size() > k_record_limit) return failure("acp_record_limit");
    if (completed && inspection.final) {
        // Native startup can finish asynchronous command discovery after the
        // prompt response. EOF settles that tail without dropping any bytes.
        if (inspection.timed_out || inspection.exit_code != 0) {
            return failure(inspection.timed_out ? "acp_timeout" : "acp_process_failed");
        }
        if (!pending->empty()) {
            return failure("acp_incomplete");
        }
        addon::Structured_inspect_result result;
        result.complete     = true;
        result.result_bytes = std::move(*output);
        return result;
    }
    if (inspection.final) return failure(inspection.timed_out ? "acp_timeout" : "acp_incomplete");
    // Never release a native request over an already observed, unclassified
    // output prefix. Complete housekeeping may be accepted, but a task reply
    // produced before its request must still be rejected on the next call.
    const bool defer_input = !pending->empty() && !input.empty();
    state.set("deferred_input_base64", Json(defer_input ? addon::encode_base64(input) : ""));
    state.set("pending_base64", Json(addon::encode_base64(*pending)));
    state.set("result_base64", Json(addon::encode_base64(*output)));
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(state);
    result.close_stdin = completed && initial_stage != "await_drain";
    if (!addon::exchange_protocol_state_valid(result.protocol_state)) return failure("acp_state_limit");
    if (!defer_input) replies += input;
    if (!replies.empty()) result.stdin_bytes = std::move(replies);
    return result;
}

} // namespace qwen
