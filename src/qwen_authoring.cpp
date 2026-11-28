#include "qwen_preparation.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/session_contracts.h>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace json  = logonomic::cli_addon::json;

// Qwen's safe mode drops external hooks, agents, extensions and MCP servers.
// Built-ins remain enabled. A whole-tool deny prevents both their registration
// and execution. Qwen 0.24.3 only reports the full registry after receiving
// the first prompt, so this inventory travels with its native release.
constexpr std::string_view k_supported_version = "0.24.3";
constexpr const char* k_excluded_tools =
    "exec,edit,write_file,read_file,zoom_image,grep_search,glob,run_shell_command,"
    "todo_write,save_memory,agent,skill,exit_plan_mode,enter_plan_mode,web_fetch,"
    "web_search,image_gen,list_directory,lsp,ask_user_question,cron_create,cron_list,"
    "cron_delete,loop_wakeup,create_sub_session,list_agents,task_stop,task_create,"
    "task_update,task_list,team_create,team_delete,team_plan_approval,request_shutdown,"
    "send_message,structured_output,monitor,notebook_edit,tool_call,tool_search,"
    "read_mcp_resource,enter_worktree,exit_worktree,workflow,artifact,record_artifact,"
    "record_source,report_findings,get_goal,update_goal,omni_downsample_image,"
    "omni_downscale_video,omni_downsample_audio,omni_extract_keyframes,"
    "omni_extract_audio,omni_clip_video,omni_convert_image,omni_transcribe_audio,"
    "omni_clip_image,omni_clip_audio,omni_caption_image,omni_caption_audio,"
    "omni_ocr_image,omni_understand_video_segments,omni_recall_media_memory,propose_goal,"
    "display_image";

bool authoring_options(const std::vector<std::string>& argv)
{
    if (argv.size() == 1) return true;
    return argv.size() == 3 && (argv[1] == "-m" || argv[1] == "--model") &&
        !argv[2].empty() && !argv[2].starts_with('-');
}

void prepare_authoring(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_prepare_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& context = request->context;
    if (!context.launch_environment || !context.launch_environment->bootstrap ||
        !authoring_options(context.launcher_argv) || request->prompt.empty() || !addon::is_valid_utf8(request->prompt))
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_launcher_unavailable");
        return;
    }
    auto profile = profile_for_context(context);
    set_environment(profile.environment, "NODE_OPTIONS", "");
    set_environment(profile.environment, "QWEN_HOME", path_string(profile.home));
    const auto& cwd = context.working_directory;
    const Launcher_probes probes(invocation);
    auto command = verified_command(context.launcher, context.launcher_argv, profile.environment, cwd,
        {"--safe-mode", "--exclude-tools", "--input-format", "--output-format"}, probes);
    if (command) {
        const auto version_argv = platform_command({command->front(), "--version"}, profile.environment, cwd);
        const auto version = version_argv ? probes.run(*version_argv, profile.environment, cwd) : std::nullopt;
        if (!version || trim(version->output) != k_supported_version) {
            invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_tool_inventory_unavailable");
            return;
        }
        command->insert(command->end(), {"--safe-mode", "--exclude-tools", k_excluded_tools,
            "--input-format", "stream-json", "--output-format", "stream-json"});
        command = platform_command(*command, profile.environment, cwd);
    }
    if (!command) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "authoring_launcher_unavailable");
        return;
    }

    const auto message = json::object({
        {"type", Json("user")}, {"session_id", Json("")}, {"parent_tool_use_id", Json()},
        {"message", json::object({{"role", Json("user")}, {"content", Json(request->prompt)}})},
    });
    addon::Structured_prepare_result result;
    result.profile_storage = context.profile_storage;
    result.account_context = context.account_context;
#ifdef _WIN32
    result.process.executable = "adapter/qwen_launch_bridge.exe";
#else
    result.process.executable = "adapter/qwen_launch_bridge";
#endif
    result.process.argv = std::move(*command);
    result.process.environment = std::move(profile.environment);
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = addon::serialize_json(message) + '\n';
    invocation.succeed(addon::encode_structured_prepare_result(result));
}

bool empty_array(const Json& value, std::string_view key)
{
    const auto& array = json::member(value, key);
    return array.is_array() && array.as_array().empty();
}

std::optional<std::string> authenticated_text(std::string_view bytes)
{
    std::string session;
    std::optional<std::string> result;
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        const auto line = bytes.substr(0, newline);
        bytes = newline == std::string_view::npos ? std::string_view{} : bytes.substr(newline + 1);
        if (line.empty() || line == "\r") continue;
        const auto frame = json::parse(line);
        if (!frame || !frame->is_object() || result) return std::nullopt;
        const auto type = json::text(*frame, "type");
        const auto subtype = json::text(*frame, "subtype");
        const auto id = json::text(*frame, "session_id");
        if (session.empty()) {
            if (type != "system" || subtype != "init" || id.empty() ||
                json::text(*frame, "qwen_code_version") != k_supported_version ||
                !empty_array(*frame, "tools") || !empty_array(*frame, "mcp_servers"))
            {
                return std::nullopt;
            }
            session = id;
            continue;
        }
        if (id != session) return std::nullopt;
        // Qwen publishes its goal snapshot even when partial messages are off.
        if (type == "stream_event" && json::member(*frame, "parent_tool_use_id").is_null() &&
            json::text(json::member(*frame, "event"), "type") == "goal_state") continue;
        if (type == "assistant") {
            const auto& message = json::member(*frame, "message");
            const auto& blocks = json::member(message, "content");
            if (!json::member(*frame, "parent_tool_use_id").is_null() ||
                json::text(message, "role") != "assistant" || !blocks.is_array()) return std::nullopt;
            for (const auto& block : blocks.as_array()) {
                const auto kind = json::text(block, "type");
                if (kind != "text" && kind != "thinking") return std::nullopt;
            }
        }
        else
        if (type == "result" && subtype == "success" && json::member(*frame, "is_error") == Json(false) &&
            empty_array(*frame, "permission_denials") && json::bounded_string(json::member(*frame, "result"),
                addon::limits::k_max_structured_stream_bytes))
        {
            result = json::text(*frame, "result");
        }
        else return std::nullopt;
    }
    return result;
}

void inspect_authoring(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_inspect_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& inspection = request->inspection;
    addon::Structured_inspect_result result;
    result.complete = true;
    if (inspection.final && !inspection.timed_out && inspection.exit_code == 0) {
        result.result_bytes = authenticated_text(inspection.stdout_bytes);
    }
    if (!result.result_bytes) result.failure_reason = "authoring_result_unavailable";
    invocation.succeed(addon::encode_structured_inspect_result(result));
}

} // namespace

void handle_authoring(addon::Invocation_context& invocation)
{
    const auto& operation = invocation.request().operation;
    if (operation == "prepare") prepare_authoring(invocation);
    else if (operation == "inspect") inspect_authoring(invocation);
    else invocation.reject_unknown_operation();
}

} // namespace qwen
