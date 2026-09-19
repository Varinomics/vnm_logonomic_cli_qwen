#include "qwen_acp.h"
#include "qwen_json.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
constexpr std::size_t k_record_limit = 256 * 1024;
constexpr std::size_t k_result_limit = 1024 * 1024;

std::string rpc(int id, const char* method, Json params)
{
    return addon::serialize_json(object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
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

bool nonnegative_integer(const Json& value)
{
    return value.is_number() && value.as_number() >= 0 && value.as_number() <= 9007199254740991.0 &&
        std::floor(value.as_number()) == value.as_number();
}

bool response(const Json& value, int expected)
{
    return exact_keys(value, {"jsonrpc", "id", "result"}) && text(value, "jsonrpc") == "2.0" &&
        member(value, "id").is_integer() && member(value, "id").as_integer() == expected &&
        member(value, "result").is_object();
}

std::optional<const char*> response_failure(const Json& value, int expected)
{
    if (!exact_keys(value, {"jsonrpc", "id", "error"}) || text(value, "jsonrpc") != "2.0" ||
        !member(value, "id").is_integer() || member(value, "id").as_integer() != expected) return std::nullopt;
    const auto& error = member(value, "error");
    if (!only_keys(error, {"code", "message", "data"}) || !member(error, "code").is_integer() ||
        !bounded_string(member(error, "message"), 16 * 1024) || !optional_object(error, "data")) {
        return "invalid_acp_error";
    }
    return member(error, "code").as_integer() == -32000 ? "authentication_required" : "acp_request_failed";
}

bool valid_initialize(const Json& result)
{
    if (!only_keys(result, {"protocolVersion", "agentCapabilities", "agentInfo", "authMethods", "_meta"}) ||
        !member(result, "protocolVersion").is_integer() || member(result, "protocolVersion").as_integer() != 1 ||
        !member(result, "agentCapabilities").is_object() || !optional_object(result, "_meta")) return false;
    const auto& info = member(result, "agentInfo");
    if (!only_keys(info, {"name", "title", "version"}) || text(info, "name") != "qwen-code" ||
        !bounded_string(member(info, "title"), 512) || !bounded_string(member(info, "version"), 512)) return false;
    const auto& methods = member(result, "authMethods");
    if (!methods.is_array() || methods.as_array().size() > 16) return false;
    std::vector<std::string> method_ids;
    for (const auto& method : methods.as_array()) {
        if (!only_keys(method, {"id", "name", "description", "_meta"}) ||
            !bounded_string(member(method, "id"), 256) ||
            !bounded_string(member(method, "name"), 512) ||
            !bounded_string(member(method, "description"), 4096) || !optional_object(method, "_meta")) return false;
        const auto id = text(method, "id");
        if (std::find(method_ids.begin(), method_ids.end(), id) != method_ids.end()) return false;
        method_ids.push_back(id);
    }
    return true;
}

bool valid_models(const Json& result)
{
    const auto& models = member(result, "models");
    if (!exact_keys(models, {"currentModelId", "availableModels"}) ||
        !bounded_string(member(models, "currentModelId"), 1024)) return false;
    const auto& available = member(models, "availableModels");
    if (!available.is_array() || available.as_array().empty() || available.as_array().size() > 1024) return false;
    bool current_found = false;
    for (const auto& model : available.as_array()) {
        if (!only_keys(model, {"modelId", "name", "description", "_meta"}) ||
            !bounded_string(member(model, "modelId"), 1024) || !bounded_string(member(model, "name"), 4096) ||
            !(member(model, "description").is_null() || bounded_string(member(model, "description"), 16 * 1024, true)) ||
            !optional_object(model, "_meta")) return false;
        current_found = current_found || text(model, "modelId") == text(models, "currentModelId");
    }
    return current_found;
}

bool valid_session(const Json& result)
{
    if (!only_keys(result, {"sessionId", "models", "modes", "configOptions", "_meta"}) ||
        !bounded_string(member(result, "sessionId"), 512) || !valid_models(result) ||
        !member(result, "modes").is_object() || !member(result, "configOptions").is_array() ||
        member(result, "configOptions").as_array().size() > 1024 || !optional_object(result, "_meta")) return false;
    return true;
}

bool valid_text_update(const Json& update)
{
    const auto& content = member(update, "content");
    return only_keys(update, {"sessionUpdate", "content", "_meta"}) && optional_object(update, "_meta") &&
        exact_keys(content, {"type", "text"}) && text(content, "type") == "text" &&
        bounded_string(member(content, "text"), k_record_limit, true);
}

bool valid_commands(const Json& update)
{
    const auto& commands = member(update, "availableCommands");
    if (!only_keys(update, {"sessionUpdate", "availableCommands", "_meta"}) || !optional_object(update, "_meta") ||
        !commands.is_array() || commands.as_array().size() > 1024) return false;
    for (const auto& command : commands.as_array()) {
        const auto& input = member(command, "input");
        if (!only_keys(command, {"name", "description", "input", "_meta"}) ||
            !bounded_string(member(command, "name"), 512) || !bounded_string(member(command, "description"), 4096) ||
            (!input.is_null() && (!exact_keys(input, {"hint"}) ||
                !bounded_string(member(input, "hint"), 4096, true))) || !optional_object(command, "_meta"))
        {
            return false;
        }
    }
    return true;
}

bool valid_tool(const Json& update)
{
    if (!only_keys(update, {"sessionUpdate", "toolCallId", "title", "kind", "status", "content", "locations",
            "rawInput", "rawOutput", "_meta"}) || !bounded_string(member(update, "toolCallId"), 512) ||
        !optional_object(update, "_meta")) return false;
    if (text(update, "sessionUpdate") == "tool_call" &&
        (!bounded_string(member(update, "title"), 4096) || !bounded_string(member(update, "status"), 64))) return false;
    for (const auto key : {"kind", "status"}) {
        if (update.find(key) && !bounded_string(member(update, key), 64)) return false;
    }
    if (update.find("title") && !bounded_string(member(update, "title"), 4096, true)) return false;
    for (const auto key : {"content", "locations"}) {
        if (update.find(key) && !member(update, key).is_array()) return false;
    }
    return true;
}

bool valid_plan(const Json& update)
{
    const auto& entries = member(update, "entries");
    if (!only_keys(update, {"sessionUpdate", "entries", "_meta"}) || !optional_object(update, "_meta") ||
        !entries.is_array() || entries.as_array().size() > 1024) return false;
    for (const auto& entry : entries.as_array()) {
        const auto priority = text(entry, "priority");
        const auto status   = text(entry, "status");
        if (!exact_keys(entry, {"content", "priority", "status"}) ||
            !bounded_string(member(entry, "content"), 16 * 1024) ||
            (priority != "low" && priority != "medium" && priority != "high") ||
            (status != "pending" && status != "in_progress" && status != "completed")) return false;
    }
    return true;
}

bool valid_standard_notification(
    const Json& value, std::string_view stage, std::string& session, std::string& output)
{
    const auto& params = member(value, "params");
    if (!exact_keys(value, {"jsonrpc", "method", "params"}) || text(value, "jsonrpc") != "2.0" ||
        text(value, "method") != "session/update" || !only_keys(params, {"sessionId", "update", "_meta"}) ||
        !bounded_string(member(params, "sessionId"), 512) || !member(params, "update").is_object() ||
        !optional_object(params, "_meta")) return false;
    const auto announced_session = text(params, "sessionId");
    if (session.empty() && stage == "await_new") session = announced_session;
    if (session != announced_session) return false;
    const auto& update = member(params, "update");
    const auto type = text(update, "sessionUpdate");
    if (type == "available_commands_update") return stage != "await_initialize" && valid_commands(update);
    if (type == "current_mode_update") {
        return (stage == "await_mode" || stage == "await_prompt") &&
            only_keys(update, {"sessionUpdate", "currentModeId", "_meta"}) && optional_object(update, "_meta") &&
            text(update, "currentModeId") == "yolo";
    }
    if (type == "config_option_update") {
        return stage != "await_initialize" && only_keys(update, {"sessionUpdate", "configOptions", "_meta"}) &&
            member(update, "configOptions").is_array() && member(update, "configOptions").as_array().size() <= 1024 &&
            optional_object(update, "_meta");
    }
    if (type == "agent_message_chunk" || type == "agent_thought_chunk" || type == "user_message_chunk") {
        if (stage != "await_prompt" || !valid_text_update(update)) return false;
        if (type == "agent_message_chunk") output += text(member(update, "content"), "text");
        return output.size() <= k_result_limit;
    }
    if (type == "tool_call" || type == "tool_call_update") return stage == "await_prompt" && valid_tool(update);
    if (type == "plan") return stage == "await_prompt" && valid_plan(update);
    if (type == "usage_update") {
        return stage == "await_prompt" && only_keys(update, {"sessionUpdate", "used", "size", "_meta"}) &&
            nonnegative_integer(member(update, "used")) && nonnegative_integer(member(update, "size")) &&
            member(update, "size").as_number() > 0 && optional_object(update, "_meta");
    }
    if (type == "session_info_update") {
        return stage == "await_prompt" && only_keys(update, {"sessionUpdate", "title", "_meta"}) &&
            (member(update, "title").is_null() || bounded_string(member(update, "title"), 4096, true)) &&
            optional_object(update, "_meta");
    }
    return false;
}

bool valid_private_notification(const Json& value, std::string_view stage, std::string& session)
{
    const auto& params = member(value, "params");
    if (!exact_keys(value, {"jsonrpc", "method", "params"}) || text(value, "jsonrpc") != "2.0" ||
        stage == "await_initialize" || !params.is_object() || !bounded_string(member(params, "sessionId"), 512)) return false;
    const auto announced_session = text(params, "sessionId");
    if (session.empty() && stage == "await_new") session = announced_session;
    if (session != announced_session) return false;
    const auto method = text(value, "method");
    if (method == "qwen/notify/session/mode-update") {
        return (stage == "await_mode" || stage == "await_prompt") &&
            exact_keys(params, {"v", "sessionId", "currentModeId"}) && member(params, "v").is_integer() &&
            member(params, "v").as_integer() == 1 && text(params, "currentModeId") == "yolo";
    }
    const bool setup_notification = method == "qwen/notify/session/mcp-budget-event" ||
        method == "qwen/notify/session/sources-changed";
    if (setup_notification) return stage == "await_new" || stage == "await_mode" || stage == "await_prompt";
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
    const auto& params = member(value, "params");
    const auto method = text(value, "method");
    if (method == "session/update") {
        const auto type = text(member(params, "update"), "sessionUpdate");
        const bool advisory = type == "available_commands_update" || type == "current_mode_update" ||
            type == "config_option_update" || type == "session_info_update";
        std::string ignored_output;
        return advisory && valid_standard_notification(value, "await_prompt", session, ignored_output);
    }
    if (!exact_keys(value, {"jsonrpc", "method", "params"}) || text(value, "jsonrpc") != "2.0" ||
        !params.is_object() || text(params, "sessionId") != session)
    {
        return false;
    }
    // SessionSourcesController uses an integer revision without a version
    // field; other native advisory producers attach v:1 explicitly.
    if (method == "qwen/notify/session/sources-changed") {
        return exact_keys(params, {"sessionId", "revision"}) && nonnegative_integer(member(params, "revision"));
    }
    if (!member(params, "v").is_integer() || member(params, "v").as_integer() != 1) {
        return false;
    }
    if (method == "qwen/notify/session/title-update") {
        return exact_keys(params, {"v", "sessionId", "title", "titleSource"}) &&
            bounded_string(member(params, "title"), 4096, true) &&
            (text(params, "titleSource") == "auto" || text(params, "titleSource") == "manual");
    }
    if (method == "qwen/notify/session/prompt-suggestion") {
        return exact_keys(params, {"v", "sessionId", "suggestion", "promptId"}) &&
            bounded_string(member(params, "suggestion"), k_record_limit) &&
            bounded_string(member(params, "promptId"), 1024);
    }
    if (method == "qwen/notify/session/pr-binding") {
        const auto& pr = member(params, "pr");
        return exact_keys(params, {"v", "sessionId", "pr"}) && exact_keys(pr, {"number", "url"}) &&
            nonnegative_integer(member(pr, "number")) && member(pr, "number").as_number() > 0 &&
            bounded_string(member(pr, "url"), 16 * 1024);
    }
    if (method == "qwen/notify/session/recording-degraded") {
        return exact_keys(params, {"v", "sessionId", "reason"}) && text(params, "reason") == "write_failed";
    }
    if (method == "qwen/notify/session/model-update") {
        return exact_keys(params, {"v", "sessionId", "currentModelId"}) &&
            bounded_string(member(params, "currentModelId"), 1024);
    }
    if (method == "qwen/notify/session/mode-update") {
        return valid_private_notification(value, "await_prompt", session);
    }
    if (method != "qwen/notify/session/mcp-budget-event" ||
        !nonnegative_integer(member(params, "liveCount")) ||
        !nonnegative_integer(member(params, "reservedCount")) ||
        !nonnegative_integer(member(params, "budget")) ||
        (params.find("scope") && text(params, "scope") != "workspace"))
    {
        return false;
    }
    if (text(params, "kind") == "budget_warning") {
        return only_keys(params, {"v", "sessionId", "scope", "kind", "liveCount", "reservedCount",
                "budget", "thresholdRatio", "mode"}) &&
            member(params, "thresholdRatio").is_number() &&
            member(params, "thresholdRatio").as_number() > 0 &&
            member(params, "thresholdRatio").as_number() <= 1 &&
            (text(params, "mode") == "warn" || text(params, "mode") == "enforce");
    }
    if (text(params, "kind") != "refused_batch" || text(params, "mode") != "enforce" ||
        !only_keys(params, {"v", "sessionId", "scope", "kind", "liveCount", "reservedCount",
            "budget", "refusedServers", "mode"}) || !member(params, "refusedServers").is_array())
    {
        return false;
    }
    for (const auto& server : member(params, "refusedServers").as_array()) {
        if (!exact_keys(server, {"name", "transport", "reason"}) ||
            !bounded_string(member(server, "name"), 1024) ||
            !bounded_string(member(server, "transport"), 64) || text(server, "reason") != "budget_exhausted")
        {
            return false;
        }
    }
    return true;
}

std::string initial_acp_input(
    const addon::Session_launch_context& context, const std::vector<addon::Delegated_task_server>& control_servers,
    std::string_view task_id)
{
    auto servers = Json::array();
    for (const auto& server : control_servers) {
        auto environment = Json::array();
        for (const auto& [name, value] : server.environment) {
            environment.push_back(object({{"name", Json(name)}, {"value", Json(value)}}));
        }
        servers.push_back(object({{"name", Json("logonomic-task-" + std::string(task_id) + "-" + server.server_id)},
            {"command", Json(server.program)}, {"args", Json::array()}, {"env", std::move(environment)}}));
    }
    return rpc(1, "initialize", object({{"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()}})) +
        rpc(2, "session/new", object({{"cwd", Json(context.working_directory)}, {"mcpServers", std::move(servers)}}));
}

} // namespace

std::string initial_acp_input(const addon::Structured_prepare_request& request)
{
    return initial_acp_input(request.context, {}, {});
}

std::string initial_acp_input(const addon::Delegated_prepare_request& request)
{
    return initial_acp_input(request.context, request.control.servers, request.control.task_id);
}

std::string initial_acp_state(std::string_view prompt)
{
    return addon::serialize_json(object({{"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")},
        {"result_base64", Json("")}}));
}

addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    const auto parsed = parse(inspection.protocol_state);
    if (!parsed || !exact_keys(*parsed, {"stage", "pending_base64", "deferred_input_base64", "prompt_base64", "session_id", "result_base64"}) ||
        !member(*parsed, "stage").is_string() || !bounded_string(member(*parsed, "session_id"), 512, true)) {
        return failure("malformed_acp_state");
    }
    auto state = *parsed;
    const auto initial_stage = text(state, "stage");
    if (initial_stage != "await_initialize" && initial_stage != "await_new" &&
        initial_stage != "await_mode" && initial_stage != "await_prompt" &&
        initial_stage != "await_drain") return failure("malformed_acp_state");
    for (const auto key : {"pending_base64", "deferred_input_base64", "prompt_base64", "result_base64"}) {
        if (!member(state, key).is_string()) return failure("malformed_acp_state");
    }
    auto pending = addon::decode_base64(text(state, "pending_base64"));
    auto deferred_input = addon::decode_base64(text(state, "deferred_input_base64"));
    auto prompt  = addon::decode_base64(text(state, "prompt_base64"));
    auto output  = addon::decode_base64(text(state, "result_base64"));
    if (!pending || !deferred_input || !prompt || !output || output->size() > k_result_limit) return failure("malformed_acp_state");
    const auto state_session = text(state, "session_id");
    if ((initial_stage == "await_initialize" && (!state_session.empty() || !output->empty())) ||
        (initial_stage == "await_new" && !output->empty()) ||
        (initial_stage == "await_mode" && (state_session.empty() || !output->empty())) ||
        ((initial_stage == "await_prompt" || initial_stage == "await_drain") &&
            (state_session.empty() || !prompt->empty())) ||
        (initial_stage == "await_drain" && output->empty())) {
        return failure("malformed_acp_state");
    }
    *pending += inspection.stdout_bytes;
    // A reply proposed while a native record is incomplete has not been
    // written yet. Retain that causal boundary across inspection calls.
    std::string input = std::move(*deferred_input);
    bool completed = initial_stage == "await_drain";
    for (;;) {
        const auto end = pending->find('\n');
        if (end == std::string::npos) break;
        if (end == 0 || end > k_record_limit) return failure("invalid_acp_record");
        const auto value = parse(std::string_view(*pending).substr(0, end));
        pending->erase(0, end + 1);
        if (!value || !value->is_object()) return failure("invalid_acp_record");
        auto stage   = text(state, "stage");
        auto session = text(state, "session_id");
        if (completed) {
            if (!valid_drain_notification(*value, session)) {
                return failure("out_of_order_acp_response");
            }
            continue;
        }
        if (value->find("method")) {
            if (value->find("id")) return failure("unsupported_acp_reverse_request");
            if (!input.empty()) {
                const auto update_type = text(member(member(*value, "params"), "update"), "sessionUpdate");
                const bool housekeeping = update_type == "available_commands_update" ||
                    update_type == "current_mode_update" || update_type == "config_option_update";
                const auto method = text(*value, "method");
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
        const int expected = stage == "await_initialize" ? 1 : stage == "await_new" ? 2 : stage == "await_mode" ? 3 : 4;
        if (const auto reason = response_failure(*value, expected)) return failure(*reason);
        if (!input.empty()) return failure("out_of_order_acp_response");
        const auto& result = member(*value, "result");
        if (stage == "await_initialize") {
            if (!response(*value, 1) || !valid_initialize(result)) return failure("invalid_acp_initialize");
            state.set("stage", Json("await_new"));
        }
        else if (stage == "await_new") {
            if (!response(*value, 2) || !valid_session(result) ||
                (!session.empty() && session != text(result, "sessionId"))) return failure("invalid_acp_session");
            state.set("session_id", member(result, "sessionId"));
            state.set("stage", Json("await_mode"));
            input = rpc(3, "session/set_mode", object({{"sessionId", member(state, "session_id")}, {"modeId", Json("yolo")}}));
        }
        else if (stage == "await_mode") {
            if (!response(*value, 3) || !result.as_object().empty() || prompt->empty() || !addon::is_valid_utf8(*prompt)) {
                return failure("invalid_acp_mode");
            }
            state.set("stage", Json("await_prompt"));
            state.set("prompt_base64", Json(""));
            auto content = Json::array();
            content.push_back(object({{"type", Json("text")}, {"text", Json(*prompt)}}));
            input = rpc(4, "session/prompt", object({{"sessionId", member(state, "session_id")},
                {"prompt", std::move(content)}}));
        }
        else if (stage == "await_prompt") {
            if (!response(*value, 4) || !exact_keys(result, {"stopReason"})) return failure("invalid_acp_completion");
            if (text(result, "stopReason") == "cancelled") return failure("acp_cancelled");
            if (text(result, "stopReason") != "end_turn" || output->empty()) return failure("invalid_acp_completion");
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
    if (!defer_input && !input.empty()) result.stdin_bytes = std::move(input);
    return result;
}

} // namespace qwen
