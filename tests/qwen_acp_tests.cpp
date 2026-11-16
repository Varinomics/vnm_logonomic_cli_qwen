#include "qwen_acp.h"
#include "qwen_acp_test_support.h"
#include "qwen_json.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
namespace wire = qwen::acp_test;
using qwen::Json;

void require(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

qwen::Json parse(std::string_view bytes)
{
    const auto value = json_util::parse(bytes);
    require(value.has_value(), "fixture JSON parses");
    return *value;
}

std::string initialize_response()
{
    return wire::initialize_response();
}

std::string new_session_response(std::string_view session = "session-alpha")
{
    return std::string(R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":")") + std::string(session) +
        R"(","models":{"currentModelId":"configured-model","availableModels":[{"modelId":"configured-model","name":"Configured model","description":null,"_meta":{"contextLimit":131072}}]},"modes":{"currentModeId":"default","availableModes":[]},"configOptions":[]}})" "\n";
}

addon::Structured_inspect_result inspect(
    std::string state, std::string stdout_bytes, bool final = false, int exit_code = 0, bool timed_out = false,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes    = std::move(stdout_bytes);
    request.final           = final;
    request.exit_code       = exit_code;
    request.timed_out       = timed_out;
    return qwen::inspect_acp(request, now);
}

std::chrono::steady_clock::time_point at(std::int64_t milliseconds)
{
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(milliseconds));
}

std::string prompt_state(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    auto response = inspect(qwen::initial_acp_state("Implement the bounded change."), initialize_response(), false, 0, false, now);
    require(!response.complete && !response.stdin_bytes, "initialize waits for the already-issued session request");
    response = inspect(response.protocol_state, new_session_response(), false, 0, false, now);
    require(!response.complete && response.stdin_bytes &&
        response.stdin_bytes->find("session/prompt") != std::string::npos,
        "authenticated session releases the prompt under launcher permissions");
    return response.protocol_state;
}

void require_waiting(const addon::Structured_inspect_result& result, const char* message)
{
    require(!result.complete && !result.close_stdin && !result.failure_reason && !result.stdin_bytes, message);
}

void require_close(const addon::Structured_inspect_result& result, std::int64_t id = 6)
{
    require(!result.complete && !result.close_stdin && !result.failure_reason && result.stdin_bytes,
        "quiescence requests conditional close without sending EOF");
    const auto request = parse(*result.stdin_bytes);
    // Built before the && chain: as its last operand, GCC 13 at -O2/-O3 reports
    // the initializer list's conditional exception cleanup as maybe-uninitialized.
    const auto expected_params = json_util::object({
        {"sessionId", Json("session-alpha")}, {"onlyIfUnheld", Json(true)},
        {"requireFlush", Json(true)}, {"drainTimeoutMs", Json(8000)}});
    require(json_util::text(request, "method") == "qwen/control/session/close" &&
        json_util::member(request, "id").as_integer() == id &&
        json_util::member(request, "params") == expected_params,
        "close requires native atomic no-holds validation and a bounded history flush");
}

void parent_end_turn_preserves_background_work()
{
    // Native Qwen returns the parent prompt while a background Agent can
    // still run; disposing its ACP Session aborts that work and its result turn.
    const auto result = inspect(prompt_state(),
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"tool_call","toolCallId":"review-agent","title":"Review change","kind":"other","status":"in_progress","rawInput":{"description":"Review the change","run_in_background":true},"_meta":{"toolName":"Agent"}}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"tool_call_update","toolCallId":"review-agent","status":"completed"}}})" "\n"
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"The review is running."}}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n");
    require(!result.complete && !result.close_stdin && !result.failure_reason,
        "a parent end_turn must keep native ACP open for background completion and synthesis");
}

void control_servers_are_session_scoped()
{
    addon::Delegated_prepare_request request;
    request.context.working_directory = "C:/work/repository";
    request.control.task_id = "task-7";
    request.control.private_artifact_root = "C:/work/repository/.task-7";
    request.control.connection = addon::Delegated_connection{
        "C:/host/logonomic_task_server_relay.exe", "C:/work/repository/.task-7/.logonomic/task-servers.json"};
    addon::Delegated_task_server server;
    server.server_id   = "control-a";
    server.program     = "C:/host/task-control.exe";
    server.actions     = {"report", "complete"};
    server.environment = {{"LOGONOMIC_TASK_CAPABILITY", "secret"}};
    request.control.servers.push_back(server);

    const auto receipts = qwen::task_server_receipts(request);
    require(receipts.size() == 1 && receipts.front().server_id == "control-a" && receipts.front().nonce.size() == 32 &&
        receipts.front().path == "C:/work/repository/.task-7/mcp/server-" + receipts.front().nonce + ".ready" &&
        receipts.front().parent_artifact == "mcp/server-" + receipts.front().nonce + ".connection",
        "each Task server gets a receipt in its own materialized folder, in the host's '/' form");
    const auto input = qwen::initial_acp_input(request, receipts);
    const auto first_end = input.find('\n');
    require(first_end != std::string::npos, "initialize is one NDJSON record");
    const auto initialize = parse(std::string_view(input).substr(0, first_end));
    const auto session = parse(std::string_view(input).substr(first_end + 1, input.size() - first_end - 2));
    require(json_util::text(initialize, "method") == "initialize", "first request initializes ACP");
    require(json_util::text(session, "method") == "session/new", "second request creates the session");
    const auto& params = json_util::member(session, "params");
    require(json_util::text(params, "cwd") == "C:/work/repository", "host working directory is preserved");
    const auto& servers = json_util::member(params, "mcpServers");
    require(servers.is_array() && servers.as_array().size() == 1, "every admitted control server is injected once");
    const auto& mapped = servers.as_array().front();
    require(json_util::text(mapped, "name") == "logonomic-task-task-7-control-a" &&
        json_util::text(mapped, "command") == "C:/host/logonomic_task_server_relay.exe",
        "task and server identity bind the host relay");
    const auto& args = json_util::member(mapped, "args");
    require(args.is_array() && args.as_array().size() == 8 &&
        args.as_array()[0].as_string() == "--descriptor" &&
        args.as_array()[1].as_string() == "C:/work/repository/.task-7/.logonomic/task-servers.json" &&
        args.as_array()[2].as_string() == "--server" && args.as_array()[3].as_string() == "control-a" &&
        args.as_array()[4].as_string() == "--attestation-path" && args.as_array()[5].as_string() == receipts.front().path &&
        args.as_array()[6].as_string() == "--attestation-nonce" && args.as_array()[7].as_string() == receipts.front().nonce,
        "the relay connects the named server and attests its catalog");
    const auto& environment = json_util::member(mapped, "env");
    require(environment.is_array() && environment.as_array().empty() && input.find("secret") == std::string::npos,
        "host control capabilities never reach ACP");
    require(input.find("modelId") == std::string::npos && input.find("authenticate") == std::string::npos,
        "preparation preserves the profile-selected auth route and model");

    addon::Structured_prepare_request structured;
    structured.context.working_directory = "C:/work/repository";
    const auto structured_input = qwen::initial_acp_input(structured);
    const auto structured_new = parse(std::string_view(structured_input).substr(
        structured_input.find('\n') + 1, structured_input.size() - structured_input.find('\n') - 2));
    require(json_util::member(json_util::member(structured_new, "params"), "mcpServers").is_array() &&
        json_util::member(json_util::member(structured_new, "params"), "mcpServers").as_array().empty(),
        "standalone structured implementation starts without delegated control servers");
}

void task_server_receipts_admit_the_session()
{
    // Preparation makes Qwen discover MCP servers before it answers
    // session/new, so the reply is where every catalog receipt must hold its
    // nonce. The relay reserves the file empty and writes the nonce only after
    // the server's catalog succeeded.
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("qwen-receipts-" + addon::random_nonce_hex());
    fs::create_directories(root / "mcp");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    addon::Delegated_prepare_request request;
    request.control.private_artifact_root = root.generic_string();
    request.control.servers.push_back({"coordination", "C:/host/coordination.exe", {"status"}, {}});
    request.control.servers.push_back({"delegation", "C:/host/delegation.exe", {"submit"}, {}});
    const auto receipts = qwen::task_server_receipts(request);
    const auto write = [](const std::string& path, const std::string& bytes) {
        std::ofstream file(addon::path_from_utf8(path), std::ios::binary | std::ios::trunc);
        file << bytes;
        require(bool(file), "receipt fixture is written");
    };
    const auto session_reply = [&]() {
        auto response = inspect(qwen::initial_acp_state("Implement it.", receipts), initialize_response());
        return inspect(response.protocol_state, new_session_response());
    };
    auto response = session_reply();
    require(response.complete && response.failure_reason == "task_servers_not_ready" && !response.stdin_bytes,
        "a Task server that never connected keeps the prompt from Qwen");
    write(receipts[0].path, receipts[0].nonce);
    write(receipts[1].path, "");
    response = session_reply();
    require(response.failure_reason == "task_servers_not_ready",
        "a reserved receipt without its nonce is a connection whose catalog did not succeed");
    write(receipts[1].path, receipts[0].nonce);
    response = session_reply();
    require(response.failure_reason == "task_servers_not_ready", "a receipt holds its own server's nonce");
    write(receipts[1].path, receipts[1].nonce + "\n");
    response = session_reply();
    require(response.failure_reason == "task_servers_not_ready", "a receipt holds exactly the nonce");
    write(receipts[1].path, receipts[1].nonce);
    response = session_reply();
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/prompt") != std::string::npos,
        "attested Task servers let the session proceed to the prompt");
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
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/title-update","params":{"v":1,"sessionId":"session-alpha","title":"Completed task","titleSource":"auto"}})" "\n");
    require_waiting(response, "prompt completion alone does not settle background work");
    response = inspect(response.protocol_state, wire::active_work(1));
    require_close(response);
    response = inspect(response.protocol_state, wire::close_response() +
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/title-update")");
    require(!response.complete && response.close_stdin && !response.result_bytes && !response.failure_reason,
        "confirmed session close requests EOF and retains the partial advisory");
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
    const auto closing = inspect(state, completion + wire::active_work(1));
    require_close(closing);
    for (std::size_t split = 0; split <= advisory.size(); ++split) {
        auto response = inspect(closing.protocol_state, wire::close_response() + advisory.substr(0, split));
        require(!response.complete && response.close_stdin && !response.failure_reason,
            "any advisory prefix keeps completion pending until EOF");
        response = inspect(response.protocol_state, advisory.substr(split), true);
        require(response.complete && response.result_bytes == "Done." && !response.failure_reason,
            "all advisory split points produce the same result at clean EOF");
    }
    auto response = inspect(closing.protocol_state, wire::close_response() + advisory, true);
    require(response.complete && response.result_bytes == "Done." && !response.close_stdin,
        "confirmed close and advisory first seen at final EOF complete without another EOF request");
    response = inspect(state, completion + wire::active_work(1) + advisory, true);
    require(response.failure_reason == "acp_incomplete",
        "clean process exit without an acknowledged conditional close cannot become success");
    // Qwen Code 0.24.3 reply, observed through the scripted delegation gate.
    response = inspect(state,
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."}}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn","_meta":{"qwen.branchPoint":{"assistantRecordUuid":"2291684c-370f-4e82-887d-35736fde30b6","checkpointUuid":"8f40be75-ec51-407d-932c-173a155bc9c7"}}}})" "\n" +
        wire::active_work(1));
    require_close(response);
    response = inspect(response.protocol_state, wire::close_response(), true);
    require(response.complete && response.result_bytes == "Done." && !response.failure_reason,
        "a prompt reply carrying Qwen's checkpoint metadata completes");
    response = inspect(state,
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."}}}})" "\n"
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn","_meta":"branch"}})" "\n", true);
    require(response.failure_reason == "invalid_acp_completion", "reply metadata must be an object");

    const auto draining = inspect(closing.protocol_state, wire::close_response()).protocol_state;
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
    auto initialize = parse(initialize_response());
    auto& result = *initialize.find("result");
    result.find("agentInfo")->set("version", Json("future-test"));
    result.set("authMethods", parse(
        R"([{"id":"future-provider","name":"Future provider","description":"Authenticate with the configured future provider"}])"));
    const auto response = inspect(qwen::initial_acp_state("Implement it."), wire::record(std::move(initialize)));
    require(!response.complete && !response.stdin_bytes && !response.failure_reason,
        "an unused future auth-method catalog does not reject native session authentication");
}

void work_accounting_negotiates_native_coverage()
{
    addon::Structured_prepare_request request;
    request.context.working_directory = "/work/repository";
    const auto input = qwen::initial_acp_input(request);
    const auto initialize = parse(std::string_view(input).substr(0, input.find('\n')));
    require(json_util::member(json_util::member(json_util::member(initialize, "params"), "_meta"),
        "qwen.daemon.activeWorkHeartbeat") == wire::active_work_capability(),
        "initialize requests all six native hold categories at the supported minimum cadence");

    auto missing = parse(initialize_response());
    missing.find("result")->erase("_meta");
    require(inspect(qwen::initial_acp_state("Task"), wire::record(missing)).failure_reason == "acp_active_work_unavailable",
        "an unnegotiated native server cannot silently retain first-end_turn completion");
    std::vector<Json> invalid;
    auto capability = wire::active_work_capability();
    capability.erase("categories");
    invalid.push_back(capability);
    capability = wire::active_work_capability();
    capability.find("categories")->push_back(Json("agent"));
    invalid.push_back(capability);
    for (const auto& interval : {Json(0), Json(4999), Json(60001), Json(5000.5), Json("5000")}) {
        capability = wire::active_work_capability();
        capability.set("intervalMs", interval);
        invalid.push_back(capability);
    }
    capability = wire::active_work_capability();
    capability.set("v", Json(2));
    invalid.push_back(capability);
    for (const auto& rejected : invalid) {
        require(inspect(qwen::initial_acp_state("Task"), wire::initialize_response(rejected)).failure_reason ==
            "acp_active_work_unavailable", "malformed native coverage fails before releasing the task");
    }

    // Released Qwen 0.24.3 echoes five categories, before native tool holds
    // were introduced. Conditional close still checks every native hold.
    capability = wire::active_work_capability();
    capability.set("categories", parse(R"(["agent","notification","shell","session","workflow"])"));
    auto released = parse(wire::initialize_response(capability));
    released.find("result")->find("agentInfo")->set("version", Json("0.24.3"));
    auto result = inspect(qwen::initial_acp_state("Task"), wire::record(released) + new_session_response());
    require(!result.failure_reason && result.stdin_bytes &&
        result.stdin_bytes->find("session/prompt") != std::string::npos,
        "Qwen 0.24.3 five-category negotiation must release the prompt");
    result = inspect(result.protocol_state, wire::message("Done.") + wire::prompt_response() + wire::active_work(1));
    require_close(result);
    result = inspect(result.protocol_state, wire::close_response(
        "session-alpha", 6, false, wire::one_hold("tool", "native-tool")));
    require_waiting(result, "native close refusal remains authoritative for an unreported hold category");
    result = inspect(result.protocol_state, wire::active_work(2));
    require_close(result, 7);
    result = inspect(result.protocol_state, wire::close_response("session-alpha", 7));
    require(result.close_stdin && !result.complete && !result.failure_reason,
        "only native close acknowledgment permits EOF with subset coverage");

    capability = wire::active_work_capability();
    capability.find("categories")->push_back(Json("future-work"));
    capability.set("futureCapability", Json(true));
    capability.set("intervalMs", Json(60000));
    const auto accepted = inspect(qwen::initial_acp_state("Task"),
        wire::active_work_snapshot(1, Json::array()) + wire::initialize_response(capability) + new_session_response());
    require(!accepted.failure_reason && accepted.stdin_bytes && accepted.stdin_bytes->find("session/prompt") != std::string::npos,
        "startup snapshots may precede initialize and additive coverage remains compatible");
    require_waiting(inspect(accepted.protocol_state, wire::message("Done.") + wire::prompt_response()),
        "the startup channel snapshot does not prove the new session is idle");
}

void default_child_type_is_not_root_output()
{
    // Qwen's SubAgentTracker uses an empty type when Agent omitted its
    // optional subagent_type; parentToolCallId still attributes the child.
    auto response = inspect(prompt_state(),
        wire::message("Default child output.", json_util::object({
            {"parentToolCallId", Json("call-agent")}, {"subagentType", Json("")}})) +
        wire::message("Parent answer.") + wire::prompt_response() + wire::active_work(1));
    require(!response.failure_reason, "native empty subagentType must not reject an authenticated child message");
    require_close(response);
    response = inspect(response.protocol_state, wire::close_response());
    response = inspect(response.protocol_state, {}, true);
    require(response.complete && !response.failure_reason && response.result_bytes == "Parent answer.",
        "an omitted native subagent type must neither fail the task nor leak child output");
}

void only_fresh_ordered_root_snapshots_allow_close()
{
    const auto initial = prompt_state(at(100000));
    const auto recorded = inspect(initial, wire::message("Done.") + wire::active_work(10), false, 0, false, at(100000));
    require_waiting(recorded, "idle observations cannot close an unfinished parent prompt");
    require_close(inspect(recorded.protocol_state, wire::prompt_response(), false, 0, false, at(114999)));
    auto expired = inspect(recorded.protocol_state, wire::prompt_response(), false, 0, false, at(115000));
    require_waiting(expired, "the observation expires exactly after three negotiated intervals");
    expired = inspect(expired.protocol_state, wire::active_work(10), false, 0, false, at(115000));
    require_waiting(expired, "a duplicate snapshot does not renew freshness");
    expired = inspect(expired.protocol_state, wire::active_work(9), false, 0, false, at(115001));
    require_waiting(expired, "a reordered snapshot does not renew freshness");
    require_close(inspect(expired.protocol_state, wire::active_work(100), false, 0, false, at(115002)));
    require_waiting(inspect(recorded.protocol_state, wire::prompt_response(), false, 0, false, at(99999)),
        "a clock preceding the observation cannot prove freshness");
    const auto negative = inspect(prompt_state(at(-100000)), wire::message("Done.") + wire::active_work(1),
        false, 0, false, at(-100000));
    require_close(inspect(negative.protocol_state, wire::prompt_response(), false, 0, false, at(-85001)));

    const auto held = inspect(initial,
        wire::message("Done.") + wire::active_work(20, "session-alpha", wire::one_hold()) + wire::prompt_response(),
        false, 0, false, at(100000));
    require_waiting(held, "a running child holds the session after the parent response");
    for (const auto sequence : {19, 20}) {
        require_waiting(inspect(held.protocol_state, wire::active_work(sequence), false, 0, false, at(100001)),
            "an old empty report cannot erase a newer child hold");
    }
    for (const auto* category : {"agent", "notification", "shell", "session", "workflow", "tool", "future-work"}) {
        auto report = parse(wire::active_work(21, "session-alpha", wire::one_hold(category)));
        report.find("params")->find("sessions")->as_array().front().set("hasRunningBackgroundTasks", Json(false));
        require_waiting(inspect(held.protocol_state, wire::record(report), false, 0, false, at(100001)),
            "every hold category prevents close even when the background-running hint is false");
    }
    require_waiting(inspect(held.protocol_state, wire::active_work_snapshot(21, Json::array()), false, 0, false, at(100001)),
        "root absence cannot stand in for an acknowledged conditional close");
    require_waiting(inspect(held.protocol_state, wire::active_work(21, "other-session"), false, 0, false, at(100001)),
        "a foreign session's empty report cannot settle the owned session");
    auto sessions = Json::array();
    sessions.push_back(wire::active_work_session("other-session", wire::one_hold()));
    sessions.push_back(wire::active_work_session("session-alpha"));
    require_close(inspect(held.protocol_state, wire::active_work_snapshot(21, sessions), false, 0, false, at(100001)));
    sessions.push_back(wire::active_work_session("session-alpha"));
    require(inspect(held.protocol_state, wire::active_work_snapshot(21, sessions)).failure_reason == "invalid_acp_active_work",
        "a contradictory duplicate session makes the full snapshot invalid");

    for (const auto& sequence : {Json(-1), Json(0), Json(1.5), Json("21")}) {
        auto report = parse(wire::active_work(21));
        report.find("params")->set("seq", sequence);
        require(inspect(held.protocol_state, wire::record(report)).failure_reason == "invalid_acp_active_work",
            "snapshot sequence identity must be a positive safe integer");
    }
    auto malformed = parse(wire::active_work(21));
    malformed.find("params")->find("sessions")->as_array().front().set("holds", Json::object());
    require(inspect(held.protocol_state, wire::record(malformed)).failure_reason == "invalid_acp_active_work",
        "unreadable holds cannot be mistaken for no work");
}

std::string background_start(std::string_view session = "session-alpha", std::string_view turn = "background-1")
{
    return wire::record(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json("admit-background")},
        {"method", Json("_qwencode/start_turn")}, {"params", json_util::object({
            {"sessionId", Json(std::string(session))}, {"source", Json("background_notification")},
            {"turnId", Json(std::string(turn))}, {"taskId", Json("child-1")}, {"kind", Json("agent")},
            {"startedAt", Json(std::int64_t{1730000000000})}, {"afterPromptId", Json("root-prompt")}})}}));
}

std::string background_response(std::string_view text, bool complete, bool partial = false,
    std::string_view turn = "background-1")
{
    return wire::message(text, json_util::object({{"source", Json("background_notification_response")},
        {"qwenDiscreteMessage", Json(true)}, {"backgroundTask", json_util::object({
            {"taskId", Json("child-1")}, {"kind", Json("agent")}, {"status", Json("completed")},
            {"turnId", Json(std::string(turn))}, {"turnComplete", Json(complete)}, {"partial", Json(partial)}})}}));
}

std::string background_end(std::string_view reason = "end_turn", std::string_view turn = "background-1")
{
    return wire::record(json_util::object({{"jsonrpc", Json("2.0")}, {"method", Json("_qwencode/end_turn")},
        {"params", json_util::object({{"sessionId", Json("session-alpha")}, {"source", Json("background_notification")},
            {"turnId", Json(std::string(turn))}, {"reason", Json(std::string(reason))}})}}));
}

void textless_intermediate_turns_retain_later_synthesis()
{
    auto result = inspect(prompt_state(), wire::message("Initial answer. ") +
        wire::active_work(1, "session-alpha", wire::one_hold()) + wire::prompt_response());
    result = inspect(result.protocol_state, background_start());
    result = inspect(result.protocol_state, background_end() +
        wire::active_work(2, "session-alpha", wire::one_hold("agent", "later-child")));
    require_waiting(result, "a textless intermediate parent turn must retain other native child work");
    const auto incomplete_state = result.protocol_state;

    result = inspect(result.protocol_state, background_start("session-alpha", "later-turn"));
    require(!result.failure_reason, "a later parent turn can consume the retained child-result history");
    result = inspect(result.protocol_state,
        wire::message("Native diagnostic display.") +
        background_response("Final synthesis.", true, false, "later-turn") +
        background_end("end_turn", "later-turn") + wire::active_work(3));
    require_close(result);
    result = inspect(result.protocol_state, wire::close_response());
    result = inspect(result.protocol_state, {}, true);
    require(result.complete && !result.failure_reason && result.result_bytes == "Initial answer. Final synthesis.",
        "later typed synthesis completes the task without native diagnostic displays");

    result = inspect(incomplete_state, wire::active_work(3));
    require_close(result);
    result = inspect(result.protocol_state, wire::close_response());
    require(result.failure_reason == "acp_background_incomplete",
        "a final textless turn still cannot establish successful synthesis");
}

void delayed_children_require_parent_synthesis()
{
    for (const bool preliminary_text : {false, true}) {
        auto result = inspect(prompt_state(),
            wire::message("Child stream is not the task answer.", json_util::object({
                {"parentToolCallId", Json("call-agent")}, {"subagentType", Json("reviewer")},
                {"agent", json_util::object({{"agentName", Json("/root/reviewer")}, {"depth", Json(1)}, {"kind", Json("subagent")}})}})) +
            (preliminary_text ? wire::message("Working. ") : "") +
            wire::active_work(1, "session-alpha", wire::one_hold()) + wire::prompt_response());
        require_waiting(result, "an empty first response is allowed while native children are running");
        // Captured from released 0.27.4: completion display precedes the
        // queued notification and has its own non-model source tag.
        result = inspect(result.protocol_state, wire::message("Background agent completed.",
            json_util::object({{"source", Json("background_task_completed")}, {"qwenDiscreteMessage", Json(true)},
                {"backgroundTask", json_util::object({{"taskId", Json("child-1")}, {"status", Json("completed")},
                    {"kind", Json("agent")}})}})));
        require_waiting(result, "native completion display is not parent synthesis or proof of quiescence");
        result = inspect(result.protocol_state, background_start());
        require(!result.complete && !result.close_stdin && !result.failure_reason && result.stdin_bytes &&
            json_util::member(json_util::member(parse(*result.stdin_bytes), "error"), "code").as_integer() == -32601,
            "native automatic-turn admission receives its documented no-host-policy response");
        result = inspect(result.protocol_state,
            wire::message("Raw task-result display.", json_util::object({{"source", Json("background_notification")}})) +
            background_response("Verified the result.", false) + wire::active_work(2));
        require_waiting(result, "even an empty heartbeat cannot erase an admitted unfinished parent turn");
        result = inspect(result.protocol_state,
            background_response("", true) + background_end() + wire::active_work(3));
        require_close(result);
        result = inspect(result.protocol_state, wire::close_response() + wire::active_work_snapshot(4, Json::array()));
        require(!result.failure_reason && result.close_stdin && !result.complete,
            "a native close acknowledgment is the first point that permits EOF");
        result = inspect(result.protocol_state, {}, true);
        require(result.complete && !result.failure_reason &&
            result.result_bytes == (preliminary_text ? "Working. Verified the result." : "Verified the result."),
            "only the parent's model text and completed synthesis become the task result");
    }

    auto pending = inspect(prompt_state(), wire::message("Preliminary answer.") +
        wire::active_work(1, "session-alpha", wire::one_hold()) + wire::prompt_response());
    pending = inspect(pending.protocol_state, background_start());
    require(!pending.failure_reason, "background failure fixtures begin with an admitted turn");
    require(inspect(pending.protocol_state, background_response("Partial answer.", true, true)).failure_reason ==
        "acp_background_incomplete", "native partial synthesis cannot produce a successful task");
    require(inspect(pending.protocol_state, background_end("cancelled")).failure_reason == "acp_cancelled",
        "background cancellation cannot be hidden by the preliminary answer");
    require(inspect(pending.protocol_state, background_response("", true) + background_end("max_tokens")).failure_reason ==
        "acp_background_incomplete", "a nonterminal synthesis stop cannot produce success");
    auto failed = inspect(pending.protocol_state,
        wire::message("[notification error] model unavailable") + background_end() + wire::active_work(2));
    require_close(failed);
    require(inspect(failed.protocol_state, wire::close_response()).failure_reason == "acp_background_incomplete",
        "native failure before the first synthesis segment lacks typed completion and cannot succeed");
    require(inspect(pending.protocol_state, background_response("Wrong turn.", true, false, "foreign-turn")).failure_reason ==
        "invalid_acp_background_turn", "synthesis completion belongs to the admitted native turn");
    require(inspect(pending.protocol_state, background_start("session-alpha", "overlapping-turn")).failure_reason ==
        "unsupported_acp_reverse_request", "a different admission cannot replace an unfinished native turn");
    require(inspect(prompt_state(), background_start("other-session")).failure_reason == "unsupported_acp_reverse_request",
        "automatic-turn admission is confined to the owned session");
}

void native_background_admission_retries_are_idempotent()
{
    // Qwen 0.27.4 times admission out after two seconds and requeues the same
    // item.turn. Captured native retries changed the RPC id from 2 to 3 while
    // retaining every admission parameter, including turnId and startedAt.
    auto request = parse(background_start());
    request.set("id", Json(2));
    auto* params = request.find("params");
    params->set("toolUseId", Json("agent-tool-call"));
    params->set("sourceTurnId", Json("root-turn"));
    params->set("label", Json("Delayed native fixture child"));
    auto pending = inspect(prompt_state(), wire::message("Working. ") +
        wire::active_work(1, "session-alpha", wire::one_hold()) + wire::prompt_response());
    pending = inspect(pending.protocol_state, wire::record(request));
    require(!pending.failure_reason && pending.stdin_bytes, "native retry fixture admits the first request");
    request.set("id", Json(3));
    auto retried = inspect(pending.protocol_state, wire::record(request));
    require(!retried.failure_reason && retried.stdin_bytes && !retried.complete && !retried.close_stdin,
        "an exact native background admission retry receives another reply without ending the session");
    const auto reply = parse(*retried.stdin_bytes);
    require(json_util::member(reply, "id").as_integer() == 3 &&
        json_util::member(json_util::member(reply, "error"), "code").as_integer() == -32601,
        "the retry reply uses its current RPC id and the documented no-host-policy outcome");
    require_waiting(inspect(retried.protocol_state, wire::active_work(2)),
        "an admission retry cannot replace typed synthesis completion with an empty heartbeat");

    for (const auto field : {"taskId", "kind", "toolUseId", "sourceTurnId", "afterPromptId", "label"}) {
        auto changed = request;
        changed.find("params")->set(field, Json("changed-identity"));
        require(inspect(pending.protocol_state, wire::record(changed)).failure_reason == "unsupported_acp_reverse_request",
            "the same turn id with contradictory native admission metadata is refused");
    }
    auto changed_time = request;
    changed_time.find("params")->set("startedAt", Json(std::int64_t{1730000000001}));
    require(inspect(pending.protocol_state, wire::record(changed_time)).failure_reason == "unsupported_acp_reverse_request",
        "the same turn id cannot change the native attempt start time");

    retried = inspect(retried.protocol_state, background_response("Verified the result.", true));
    require_waiting(retried, "typed synthesis still waits for its native end boundary");
    request.set("id", Json("late-retry"));
    auto& reordered = request.find("params")->as_object();
    std::reverse(reordered.begin(), reordered.end());
    retried = inspect(retried.protocol_state, wire::record(request));
    require(!retried.failure_reason && retried.stdin_bytes && !retried.complete && !retried.close_stdin,
        "retry identity is independent of JSON member order and preserves existing completion evidence");
    retried = inspect(retried.protocol_state, background_end() + wire::active_work(3));
    require_close(retried);
    retried = inspect(retried.protocol_state, wire::close_response(), true);
    require(!retried.failure_reason && retried.complete && retried.result_bytes == "Working. Verified the result.",
        "an admission retry does not reset completion or duplicate parent synthesis");
}

void conditional_close_handles_races_and_invalid_responses()
{
    const auto initial = prompt_state();
    const auto closing = inspect(initial, wire::message("Done.") + wire::active_work(1) + wire::prompt_response());
    require_close(closing);
    auto refused = inspect(closing.protocol_state,
        wire::close_response("session-alpha", 6, false, wire::one_hold("notification", "parent-synthesis")));
    require_waiting(refused, "work starting after the empty snapshot makes native close defer");
    refused = inspect(refused.protocol_state, wire::active_work(1));
    require_waiting(refused, "close refusal invalidates every already-observed snapshot");
    const auto retried = inspect(refused.protocol_state, wire::active_work(2));
    require_close(retried, 7);
    require(inspect(retried.protocol_state, wire::close_response()).failure_reason == "invalid_acp_close",
        "a late reply to an earlier close cannot acknowledge a later close");
    const auto settled = inspect(retried.protocol_state, wire::close_response("session-alpha", 7), true);
    require(settled.result_bytes == "Done." && !settled.failure_reason, "a fresh retry may settle the same native session");
    require(inspect(closing.protocol_state, wire::close_response("foreign")).failure_reason == "invalid_acp_close",
        "close acknowledgments authenticate the native session");
    require(inspect(closing.protocol_state, wire::close_response("session-alpha", 6, false)).failure_reason == "invalid_acp_close",
        "a refusal without its native hold evidence is malformed");
    require(inspect(closing.protocol_state,
        R"({"jsonrpc":"2.0","id":6,"error":{"code":-32601,"message":"Method not found"}})" "\n").failure_reason ==
        "acp_request_failed", "unsupported conditional close fails instead of falling back to EOF");
    const auto admission_race = inspect(closing.protocol_state, background_start());
    require(!admission_race.failure_reason && admission_race.stdin_bytes && !admission_race.close_stdin,
        "native background admission stays live while conditional close is outstanding");
    require(inspect(admission_race.protocol_state, wire::close_response()).failure_reason == "invalid_acp_close",
        "an admitted unfinished parent turn contradicts a successful close acknowledgment");

    const auto partial = inspect(initial, wire::message("Done.") + wire::active_work(1) + wire::prompt_response() +
        R"({"jsonrpc":"2.0","method":"qwen/notify/channel/active-work")");
    require_waiting(partial, "an unclassified trailing record holds the conditional close request");
    const auto held_tail = inspect(partial.protocol_state,
        R"(,"params":{"v":1,"seq":2,"sessions":[{"sessionId":"session-alpha","holds":[{"category":"agent","id":"new-child"}]}]}})" "\n");
    require_waiting(held_tail, "a fragmented newer child snapshot wins before any close is sent");
    require(inspect(initial, wire::message("Done.") + wire::active_work(1) + wire::prompt_response() +
        wire::close_response()).failure_reason == "out_of_order_acp_response",
        "a close response produced before its request cannot authenticate completion");
}

void native_prompt_metadata_is_advisory()
{
    // Released 0.27.4 emits this asynchronously when its recording service
    // saves a user prompt; it is metadata, not another prompt or model text.
    const auto initial = prompt_state();
    const auto waiting = inspect(initial, wire::message("Done.") + wire::prompt_response());
    const auto closing = inspect(waiting.protocol_state, wire::active_work(1));
    require_close(closing);
    const auto draining = inspect(closing.protocol_state, wire::close_response());
    auto notification = parse(
        R"({"jsonrpc":"2.0","method":"qwen/notify/session/last-user-prompt","params":{"v":1,"sessionId":"session-alpha","timestamp":"2026-11-15T12:30:25.027Z"}})");
    for (const auto& phase : {initial, waiting.protocol_state, closing.protocol_state, draining.protocol_state}) {
        require_waiting(inspect(phase, wire::record(notification)),
            "native last-prompt metadata is accepted in every live and drain phase");
        auto empty = notification;
        empty.find("params")->set("timestamp", Json());
        require_waiting(inspect(phase, wire::record(empty)), "native absent timestamp remains a typed advisory");
        auto malformed = notification;
        malformed.find("params")->set("timestamp", Json(true));
        require(inspect(phase, wire::record(malformed)).failure_reason.has_value(),
            "last-prompt metadata rejects an untyped timestamp");
        malformed = notification;
        malformed.find("params")->set("v", Json(2));
        require(inspect(phase, wire::record(malformed)).failure_reason.has_value(),
            "last-prompt metadata requires its native schema version");
        malformed = notification;
        malformed.find("params")->set("sessionId", Json("foreign-session"));
        require(inspect(phase, wire::record(malformed)).failure_reason.has_value(),
            "last-prompt metadata cannot cross native session identity");
    }
    const auto final = inspect(draining.protocol_state, wire::record(notification), true);
    require(final.result_bytes == "Done." && !final.failure_reason,
        "late persisted prompt metadata does not alter the authenticated model answer");
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
    require(response.complete && response.failure_reason == "task_permission_response_unavailable",
        "malformed permission requests fail without approval");

    const std::string drain_request =
        R"({"jsonrpc":"2.0","id":0,"method":"craft/drainMidTurnQueue","params":{"sessionId":"session-alpha","todoStopGuardWatchQueuedPrompt":true}})" "\n";
    response = inspect(state, drain_request);
    require(!response.complete && !response.failure_reason && response.stdin_bytes ==
        std::string(R"({"jsonrpc":"2.0","id":0,"error":{"code":-32601,"message":"Method not found"}})") + "\n",
        "Qwen's mid-turn queue request is answered as a client without that queue");
    response = inspect(state, drain_request + R"({"jsonrpc":"2.0","method":"session/update")");
    require(!response.complete && !response.failure_reason && response.stdin_bytes &&
        response.stdin_bytes->find("-32601") != std::string::npos,
        "a fully received native request is answered while a later record is partial");
    auto foreign_drain = drain_request;
    foreign_drain.replace(foreign_drain.find("session-alpha"), std::string("session-alpha").size(), "session-other");
    response = inspect(state, foreign_drain);
    require(response.complete && response.failure_reason == "unsupported_acp_reverse_request",
        "a mid-turn queue request for another session is refused");
    response = inspect(state,
        R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"cancelled"}})" "\n");
    require(response.complete && response.failure_reason == "acp_cancelled" && !response.result_bytes,
        "Qwen's source-defined cancelled stop reason cannot become success");

    std::string catalog = R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"session-alpha","update":{"sessionUpdate":"available_commands_update","availableCommands":[)";
    for (int index = 0; catalog.size() < 600 * 1024; ++index) {
        catalog += std::string(index ? "," : "") + R"({"name":"skill-)" + std::to_string(index) +
            R"(","description":")" + std::string(1000, 'd') + R"(","input":null})";
    }
    catalog += "]}}}\n";
    response = inspect(state, catalog);
    require(!response.complete && !response.failure_reason,
        "a command catalog the size Qwen announces in a fresh profile is one acceptable record");

    response = inspect(state, std::string(1024 * 1024 + 1, 'x'));
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
    require(!response.complete && response.stdin_bytes && response.stdin_bytes->find("session/prompt") != std::string::npos,
        "ordered coalesced responses advance exactly one outgoing action");
}

} // namespace

int main()
{
    parent_end_turn_preserves_background_work();
    control_servers_are_session_scoped();
    task_server_receipts_admit_the_session();
    authenticated_transcript_completes();
    completion_is_independent_of_stdout_chunks();
    authentication_catalog_is_advisory();
    work_accounting_negotiates_native_coverage();
    default_child_type_is_not_root_output();
    only_fresh_ordered_root_snapshots_allow_close();
    textless_intermediate_turns_retain_later_synthesis();
    delayed_children_require_parent_synthesis();
    native_background_admission_retries_are_idempotent();
    conditional_close_handles_races_and_invalid_responses();
    native_prompt_metadata_is_advisory();
    failures_are_terminal_and_bounded();
    response_order_is_authenticated();
    return 0;
}
