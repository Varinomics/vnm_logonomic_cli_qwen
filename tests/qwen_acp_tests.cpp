#include "qwen_acp.h"
#include "qwen_json.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

qwen::Json parse(std::string_view bytes)
{
    const auto value = qwen::parse(bytes);
    require(value.has_value(), "fixture JSON parses");
    return *value;
}

std::string initialize_response()
{
    // packages/cli/src/acp-integration/acpAgent.ts at upstream 6a211b4:
    // initialize identifies qwen-code and advertises these two terminal auth methods.
    return R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"agentInfo":{"name":"qwen-code","title":"Qwen Code","version":"0.0-test"},"authMethods":[{"id":"openai","name":"Use OpenAI API key","description":"Requires OPENAI_API_KEY","_meta":{"type":"terminal","args":["--auth-type=openai"]}},{"id":"openai-responses","name":"Use OpenAI Responses API key","description":"Requires OPENAI_API_KEY","_meta":{"type":"terminal","args":["--auth-type=openai-responses"]}}],"agentCapabilities":{}}})" "\n";
}

std::string new_session_response(std::string_view session = "session-alpha")
{
    return std::string(R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":")") + std::string(session) +
        R"(","models":{"currentModelId":"configured-model","availableModels":[{"modelId":"configured-model","name":"Configured model","description":null,"_meta":{"contextLimit":131072}}]},"modes":{"currentModeId":"default","availableModes":[]},"configOptions":[]}})" "\n";
}

addon::Structured_inspect_result inspect(
    std::string state, std::string stdout_bytes, bool final = false, int exit_code = 0, bool timed_out = false)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes    = std::move(stdout_bytes);
    request.final           = final;
    request.exit_code       = exit_code;
    request.timed_out       = timed_out;
    return qwen::inspect_acp(request);
}

std::string prompt_state()
{
    auto response = inspect(qwen::initial_acp_state("Implement the bounded change."), initialize_response());
    require(!response.complete && !response.stdin_bytes, "initialize waits for the already-issued session request");
    response = inspect(response.protocol_state, new_session_response());
    require(!response.complete && response.stdin_bytes &&
        response.stdin_bytes->find("session/set_mode") != std::string::npos &&
        response.stdin_bytes->find("yolo") != std::string::npos,
        "authenticated session selects Qwen's noninteractive implementation mode");
    response = inspect(response.protocol_state,
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/mode-update","params":{"v":1,"sessionId":"session-alpha","currentModeId":"yolo"}})" "\n"
        R"({"jsonrpc":"2.0","id":3,"result":{}})" "\n");
    require(!response.complete && response.stdin_bytes &&
        response.stdin_bytes->find("session/prompt") != std::string::npos,
        "successful mode selection releases the prompt");
    return response.protocol_state;
}

void control_servers_are_session_scoped()
{
    addon::Delegated_prepare_request request;
    request.context.working_directory = "C:/work/repository";
    request.control.task_id = "task-7";
    addon::Delegated_task_server server;
    server.server_id   = "control-a";
    server.program     = "C:/host/task-control.exe";
    server.actions     = {"report", "complete"};
    server.environment = {{"LOGONOMIC_TASK_CAPABILITY", "secret"}};
    request.control.servers.push_back(server);

    const auto input = qwen::initial_acp_input(request);
    const auto first_end = input.find('\n');
    require(first_end != std::string::npos, "initialize is one NDJSON record");
    const auto initialize = parse(std::string_view(input).substr(0, first_end));
    const auto session = parse(std::string_view(input).substr(first_end + 1, input.size() - first_end - 2));
    require(qwen::text(initialize, "method") == "initialize", "first request initializes ACP");
    require(qwen::text(session, "method") == "session/new", "second request creates the session");
    const auto& params = qwen::member(session, "params");
    require(qwen::text(params, "cwd") == "C:/work/repository", "host working directory is preserved");
    const auto& servers = qwen::member(params, "mcpServers");
    require(servers.is_array() && servers.as_array().size() == 1, "every admitted control server is injected once");
    const auto& mapped = servers.as_array().front();
    require(qwen::text(mapped, "name") == "logonomic-task-task-7-control-a" &&
        qwen::text(mapped, "command") == "C:/host/task-control.exe", "task and server identity bind the native command");
    require(qwen::member(mapped, "args").is_array() && qwen::member(mapped, "args").as_array().empty(),
        "SDK task servers expose an executable without invented arguments");
    const auto& environment = qwen::member(mapped, "env");
    require(environment.is_array() && environment.as_array().size() == 1 &&
        qwen::text(environment.as_array().front(), "name") == "LOGONOMIC_TASK_CAPABILITY" &&
        qwen::text(environment.as_array().front(), "value") == "secret",
        "host-issued control capability reaches only its MCP child");
    require(input.find("modelId") == std::string::npos && input.find("authenticate") == std::string::npos,
        "preparation preserves the profile-selected auth route and model");

    addon::Structured_prepare_request structured;
    structured.context.working_directory = "C:/work/repository";
    const auto structured_input = qwen::initial_acp_input(structured);
    const auto structured_new = parse(std::string_view(structured_input).substr(
        structured_input.find('\n') + 1, structured_input.size() - structured_input.find('\n') - 2));
    require(qwen::member(qwen::member(structured_new, "params"), "mcpServers").is_array() &&
        qwen::member(qwen::member(structured_new, "params"), "mcpServers").as_array().empty(),
        "standalone structured implementation starts without delegated control servers");
}

void authenticated_transcript_completes()
{
    auto response = inspect(prompt_state(),
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"user_message_chunk","content":{"type":"text","text":"Implement the bounded change."},"_meta":{"timestamp":1}}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Checking."}}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"tool_call","toolCallId":"call-1","title":"Edit file","kind":"edit","status":"pending"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."},"_meta":{"source":"live"}}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"usage_update","used":25,"size":131072}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"session_info_update","title":"Completed task"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/title-update","params":{"v":1,"sessionId":"session-alpha","title":"Completed task","titleSource":"auto"}})" "\n"
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/title-update")");
    require(!response.complete && response.close_stdin && !response.result_bytes && !response.failure_reason,
        "authenticated prompt completion requests EOF and retains the partial advisory");
    response = inspect(response.protocol_state,
        R"(,"params":{"v":1,"sessionId":"session-alpha","title":"Completed task","titleSource":"auto"}})" "\n");
    require(!response.complete && !response.close_stdin && !response.failure_reason,
        "the later advisory completes under the same drain state");
    response = inspect(response.protocol_state, {}, true);
    require(response.complete && response.result_bytes == "Done." && !response.failure_reason &&
        response.protocol_state.empty(), "only clean native EOF releases authenticated Qwen agent text");
}

void completion_is_independent_of_stdout_chunks()
{
    // Native acpAgent schedules sendAvailableCommandsUpdate without awaiting
    // it; Session builds the command catalog asynchronously before sending.
    const std::string completion =
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."}}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n";
    const std::string advisory =
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"available_commands_update","availableCommands":[{"name":"help","description":"Show commands","input":null}]}}})" "\n";
    const auto state = prompt_state();
    for (std::size_t split = 0; split <= advisory.size(); ++split) {
        auto response = inspect(state, completion + advisory.substr(0, split));
        require(!response.complete && response.close_stdin && !response.failure_reason,
            "any advisory prefix keeps completion pending until EOF");
        response = inspect(response.protocol_state, advisory.substr(split), true);
        require(response.complete && response.result_bytes == "Done." && !response.failure_reason,
            "all advisory split points produce the same result at clean EOF");
    }
    auto response = inspect(state, completion + advisory, true);
    require(response.complete && response.result_bytes == "Done." && !response.close_stdin,
        "prompt and advisory first seen at final EOF complete without another close request");

    const auto draining = inspect(state, completion).protocol_state;
    response = inspect(draining, advisory.substr(0, advisory.size() - 1), true);
    require(response.failure_reason == "acp_incomplete", "an incomplete final advisory is not discarded");
    response = inspect(draining, {}, true, 7);
    require(response.failure_reason == "acp_process_failed", "failed native shutdown cannot authenticate success");
    response = inspect(draining, {}, true, 0, true);
    require(response.failure_reason == "acp_timeout", "EOF settlement retains the host deadline");
    response = inspect(draining, completion);
    require(response.failure_reason == "out_of_order_acp_response", "post-result content cannot enter the result");
    response = inspect(draining, R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n");
    require(response.failure_reason == "out_of_order_acp_response", "a repeated prompt response is rejected");
    response = inspect(draining,
        R"({"jsonrpc":"2.0","id":9,"method":"session/request_permission","params":{"sessionId":"session-alpha"}})" "\n");
    require(response.failure_reason == "out_of_order_acp_response", "drain cannot accept reverse requests");
    auto foreign = advisory;
    foreign.replace(foreign.find("session-alpha"), std::string("session-alpha").size(), "session-other");
    response = inspect(draining, foreign);
    require(response.failure_reason == "out_of_order_acp_response", "drain authenticates the advisory session");
    response = inspect(draining,
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/title-update","params":{"v":1,"sessionId":"session-alpha"}})" "\n");
    require(response.failure_reason == "out_of_order_acp_response", "a method name alone does not validate an advisory");
    response = inspect(draining,
        R"({"jsonrpc":"2.0","method":"_qwencode/start_turn","params":{"sessionId":"session-alpha"}})" "\n");
    require(response.failure_reason == "out_of_order_acp_response", "a new turn is not drain housekeeping");
    response = inspect(draining,
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/generation/event","params":{"sessionId":"session-alpha"}})" "\n");
    require(response.failure_reason == "out_of_order_acp_response", "generation events cannot extend a completed prompt");
    response = inspect(draining,
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/sources-changed","params":{"sessionId":"session-alpha","revision":2}})" "\n"
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/mcp-budget-event","params":{"v":1,"sessionId":"session-alpha","kind":"budget_warning","liveCount":3,"reservedCount":4,"budget":5,"thresholdRatio":0.8,"mode":"warn"}})" "\n", true);
    require(response.result_bytes == "Done." && !response.failure_reason,
        "native source revisions and typed MCP budget advisories can settle before EOF");
}

void authentication_catalog_is_advisory()
{
    const auto response = inspect(qwen::initial_acp_state("Implement it."),
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"agentInfo":{"name":"qwen-code","title":"Qwen Code","version":"future-test"},"authMethods":[{"id":"future-provider","name":"Future provider","description":"Authenticate with the configured future provider"}],"agentCapabilities":{}}})" "\n");
    require(!response.complete && !response.stdin_bytes && !response.failure_reason,
        "an unused future auth-method catalog does not reject native session authentication");
}

void failures_are_terminal_and_bounded()
{
    auto response = inspect(qwen::initial_acp_state("Implement it."), initialize_response());
    response = inspect(response.protocol_state,
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Use Qwen Code CLI to authenticate first.","data":{"authMethods":[]}}})" "\n");
    require(response.complete && response.failure_reason == "authentication_required" && !response.result_bytes,
        "native auth-required errors keep their host-known failure meaning");

    const auto state = prompt_state();
    response = inspect(state,
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"foreign","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"untrusted"}}}})" "\n");
    require(response.complete && response.failure_reason == "unauthenticated_acp_notification",
        "foreign session output cannot enter the result");

    response = inspect(state,
        R"({"jsonrpc":"2.0","id":91,"method":"session/request_permission","params":{"sessionId":"session-alpha"}})" "\n");
    require(response.complete && response.failure_reason == "unsupported_acp_reverse_request",
        "noninteractive delegated execution refuses reverse authority requests");

    response = inspect(state,
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"cancelled"}})" "\n");
    require(response.complete && response.failure_reason == "acp_cancelled" && !response.result_bytes,
        "Qwen's source-defined cancelled stop reason cannot become success");

    response = inspect(state, std::string(256 * 1024 + 1, 'x'));
    require(response.complete && response.failure_reason == "acp_record_limit",
        "unterminated native output remains bounded");

    addon::Structured_inspection final;
    final.protocol_state = state;
    final.final           = true;
    final.timed_out       = true;
    response = qwen::inspect_acp(final);
    require(response.complete && response.failure_reason == "acp_timeout",
        "host-owned process timeout cannot be mistaken for an ACP result");
}

void response_order_is_authenticated()
{
    auto response = inspect(qwen::initial_acp_state("Implement it."), new_session_response());
    require(response.complete && response.failure_reason == "invalid_acp_initialize",
        "a session response cannot stand in for initialize");

    response = inspect(qwen::initial_acp_state("Implement it."), initialize_response() + new_session_response());
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/set_mode") != std::string::npos,
        "ordered coalesced responses advance exactly one outgoing action");
}

} // namespace

int main()
{
    control_servers_are_session_scoped();
    authenticated_transcript_completes();
    completion_is_independent_of_stdout_chunks();
    authentication_catalog_is_advisory();
    failures_are_terminal_and_bounded();
    response_order_is_authenticated();
    return 0;
}
