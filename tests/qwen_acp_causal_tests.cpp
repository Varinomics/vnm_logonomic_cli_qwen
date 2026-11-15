#include "qwen_acp.h"
#include "qwen_acp_test_support.h"
#include "qwen_json.h"
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
namespace addon = logonomic::cli_addon;
namespace json_util = logonomic::cli_addon::json;
namespace provider = qwen;
namespace wire = qwen::acp_test;
namespace {
int assertions=0, failures=0;
void require(bool ok, const char* message) {
    ++assertions;
    if (!ok && failures++ < 8) std::cerr << "FAIL: " << message << '\n';
}
addon::Structured_inspect_result inspect(std::string state, std::string bytes,
    bool final=false, int exit_code=0, bool timed_out=false) {
    addon::Structured_inspection r;
    r.protocol_state=std::move(state);r.stdout_bytes=std::move(bytes);
    r.final=final;r.exit_code=exit_code;r.timed_out=timed_out;
    return provider::inspect_acp(r);
}
const std::string initialize = wire::initialize_response();
const std::string new_session=R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"s","models":{"currentModelId":"m","availableModels":[{"modelId":"m","name":"Model","description":null}]},"modes":{},"configOptions":[]}})" "\n";
const std::string completion=R"({"jsonrpc":"2.0","id":4,"result":{"stopReason":"end_turn"}})" "\n";
const std::string model_text=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"ANSWER"}}}})" "\n";
const std::string commands=R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"available_commands_update","availableCommands":[]}}})" "\n";
}

void launcher_permissions()
{
    using Json = provider::Json;
    addon::Delegated_prepare_request request;
    request.prompt = "Task";
    request.control.task_id = "6a4b79a8-24d9-478c-8ee9-94f93151c2a7";
    request.control.private_artifact_root = "C:/task";
    request.control.servers.push_back({"delegation", "relay", {"submit"}, {}});
    const auto initialized = inspect(provider::initial_acp_state(request.prompt, provider::task_server_receipts(request)), initialize);
    auto state = *json_util::parse(initialized.protocol_state);
    state.set("stage", Json("await_prompt"));
    state.set("session_id", Json("s"));
    state.set("prompt_base64", Json(""));
    const auto pending = addon::serialize_json(state);
    const auto permission = [](std::string name, std::string session = "s", bool once = true) {
        auto call = json_util::object({{"toolCallId", Json("call-1")}});
        call.set("title", Json("arguments")); call.set("_meta", json_util::object({{"toolName", Json(name)}}));
        auto options = Json::array();
        options.push_back(json_util::object({{"optionId", Json("always")}, {"kind", Json("allow_always")}}));
        options.push_back(json_util::object({{"optionId", Json("proceed_once_and_switch_to_default")},
            {"kind", Json("allow_once")}}));
        if (once) options.push_back(json_util::object({{"optionId", Json("proceed_once")}, {"kind", Json("allow_once")}}));
        options.push_back(json_util::object({{"optionId", Json("cancel")}, {"kind", Json("reject_once")}}));
        return addon::serialize_json(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(17)},
            {"method", Json("session/request_permission")}, {"params", json_util::object({
                {"sessionId", Json(session)}, {"toolCall", std::move(call)}, {"options", std::move(options)}})}})) + '\n';
    };
    const std::string own = "mcp__logonomic-task-6a4b79a8-24d9-478c-8ee9-94f93151c2a_15ssf2z";
    const auto waiting = inspect(pending, model_text + wire::active_work(1, "s", wire::one_hold()) + completion);
    const auto closing = inspect(pending, model_text + wire::active_work(1, "s") + completion);
    require(!waiting.failure_reason && !closing.failure_reason && closing.stdin_bytes,
        "permission fixtures cover parent generation, background work, and outstanding close");
    for (const auto& phase : {pending, waiting.protocol_state, closing.protocol_state}) {
        for (const auto& name : {own, std::string("Agent"), std::string("agent"), std::string("run_shell_command"), own + "_other",
                std::string("mcp__user_server__submit")})
        {
            const auto result = inspect(phase, permission(name) + "{");
            const auto expected = name == own ? "proceed_once" : "cancel";
            require(!result.failure_reason && result.stdin_bytes &&
                result.stdin_bytes->find(std::string("\"optionId\":\"") + expected + "\"") != std::string::npos,
                "permission replies immediately in every live phase: own Task tool once, everything else rejected once");
            const auto next = inspect(result.protocol_state, "");
            require(!next.stdin_bytes, "a permission reply is not replayed across a partial later record");
        }
    }
    require(inspect(pending, permission(own, "foreign")).failure_reason.has_value(),
        "a foreign session cannot obtain an approval");
    require(inspect(pending, permission(own, "s", false)).failure_reason.has_value(),
        "missing plain allow-once refuses instead of selecting a persistent or mode-changing approval");
}

void task_model_selection()
{
    const addon::Delegated_model_selection selection{"task-model", "high"};
    const auto setup = inspect(provider::initial_acp_state("TASK PAYLOAD", {}, &selection), initialize + new_session);
    require(!setup.failure_reason && setup.stdin_bytes && setup.stdin_bytes->find("session/set_config_option") != std::string::npos &&
        setup.stdin_bytes->find(R"("configId":"reasoning_effort","value":"high")") != std::string::npos &&
        setup.stdin_bytes->find("_meta") == std::string::npos && setup.stdin_bytes->find("TASK PAYLOAD") == std::string::npos,
        "effort is configured per session without persistence metadata before the task");
    const std::string acknowledged = R"({"jsonrpc":"2.0","id":3,"result":{"configOptions":[{"id":"reasoning_effort","currentValue":"high"}]}})" "\n";
    for (std::size_t split = 1; split < acknowledged.size(); ++split) {
        const auto first = inspect(setup.protocol_state, acknowledged.substr(0, split));
        require(!first.failure_reason && !first.stdin_bytes, "partial effort acknowledgment holds the task");
        const auto second = inspect(first.protocol_state, acknowledged.substr(split));
        require(!second.failure_reason && second.stdin_bytes && second.stdin_bytes->find("TASK PAYLOAD") != std::string::npos &&
            second.stdin_bytes->find(R"("id":5)") != std::string::npos, "exact effort acknowledgment releases the task once");
    }
    const auto prompt = inspect(setup.protocol_state, acknowledged);
    const auto finished = inspect(prompt.protocol_state, model_text +
        wire::active_work(1, "s") + wire::prompt_response(5));
    require(!finished.failure_reason && !finished.close_stdin && finished.stdin_bytes &&
        finished.stdin_bytes->find("qwen/control/session/close") != std::string::npos,
        "configured task also settles native background work before close");
    const auto closed = inspect(finished.protocol_state, wire::close_response("s"));
    require(!closed.failure_reason && closed.close_stdin && inspect(closed.protocol_state, "", true).result_bytes == "ANSWER",
        "configured task retains acknowledged-close and clean-exit completion");
    require(inspect(setup.protocol_state,
        R"({"jsonrpc":"2.0","id":3,"result":{"configOptions":[{"id":"reasoning_effort","currentValue":"medium"}]}})" "\n").failure_reason ==
        "reasoning_effort_unavailable", "a clamped effort never releases the task");
    require(inspect(setup.protocol_state,
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32602,"message":"Unsupported effort"}})" "\n").failure_reason ==
        "reasoning_effort_unavailable", "native unsupported effort is a scoped refusal");
    require(inspect(setup.protocol_state, acknowledged + model_text).failure_reason.has_value(),
        "a pre-request answer cannot authenticate a configured task");
    const std::string changed = R"({"jsonrpc":"2.0","method":"qwen/notify/session/model-update","params":{"v":1,"sessionId":"s","currentModelId":"other"}})" "\n";
    require(inspect(prompt.protocol_state, changed).failure_reason == "model_selection_unavailable",
        "an observed change to an explicitly requested model cannot silently succeed");
    const addon::Delegated_model_selection effort_only{std::nullopt, "high"};
    const auto effort = inspect(provider::initial_acp_state("TASK PAYLOAD", {}, &effort_only), initialize + new_session);
    require(!effort.failure_reason && effort.stdin_bytes == setup.stdin_bytes,
        "effort-only selection uses the native default model");
}

int main() {
    launcher_permissions();
    task_model_selection();
    const auto setup=inspect(provider::initial_acp_state("TASK PAYLOAD"),initialize);
    require(!setup.stdin_bytes && !setup.failure_reason, "initialize awaits the session");
    // A whole frame and every strict prefix have the same causal meaning.
    for (const auto& forbidden : {model_text, completion}) {
        const auto whole=inspect(setup.protocol_state,new_session+forbidden);
        require(whole.failure_reason.has_value() && !whole.stdin_bytes,"whole pre-request frame is rejected before task release");
        for (std::size_t split=1;split<forbidden.size();++split) {
            const auto first=inspect(setup.protocol_state,new_session+forbidden.substr(0,split));
            require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"unclassified pre-request prefix holds task");
            const auto empty=inspect(first.protocol_state,"");
            require(!empty.complete&&!empty.failure_reason&&!empty.stdin_bytes,"empty inspection cannot release deferred task");
            const auto second=inspect(empty.protocol_state,forbidden.substr(split));
            require(second.failure_reason.has_value()&&!second.stdin_bytes,"forbidden tail retains pre-request provenance");
        }
    }
    const auto full_housekeeping=inspect(setup.protocol_state,new_session+commands);
    require(!full_housekeeping.failure_reason&&full_housekeeping.stdin_bytes.has_value(),"complete housekeeping permits prompt");
    for (std::size_t split=1;split<commands.size();++split) {
        const auto first=inspect(setup.protocol_state,new_session+commands.substr(0,split));
        require(!first.complete&&!first.failure_reason&&!first.stdin_bytes,"partial housekeeping holds task");
        const auto second=inspect(first.protocol_state,commands.substr(split));
        require(!second.failure_reason&&second.stdin_bytes.has_value(),"classified housekeeping releases task exactly once");
        const auto third=inspect(second.protocol_state,"");
        require(!third.complete&&!third.failure_reason&&!third.stdin_bytes,"deferred task is not replayed");
    }
    const auto prompt=inspect(setup.protocol_state,new_session);
    require(!prompt.failure_reason&&prompt.stdin_bytes && prompt.stdin_bytes->find("session/prompt") != std::string::npos,"prompt keeps launcher permission mode");
    auto wrong_session=model_text; wrong_session.replace(wrong_session.find("\"s\""),3,"\"other\"");
    require(inspect(prompt.protocol_state,wrong_session).failure_reason.has_value(),"foreign session cannot supply result");
    const std::string reverse=R"({"jsonrpc":"2.0","id":20,"method":"session/request_permission","params":{"sessionId":"s"}})" "\n";
    require(inspect(prompt.protocol_state,reverse).failure_reason.has_value(),"unsupported reverse request is not a notification");
    require(inspect(prompt.protocol_state,std::string(1024*1024+1,'x')).failure_reason.has_value(),"unterminated record is bounded");
    require(inspect(prompt.protocol_state,"",true,0,true).failure_reason.has_value(),"timeout cannot complete a task");
    for (std::size_t split=1;split<model_text.size();++split) {
        const auto first=inspect(prompt.protocol_state,model_text.substr(0,split));
        const auto second=inspect(first.protocol_state,model_text.substr(split)+wire::active_work(1,"s")+completion);
        require(!first.failure_reason&&!second.failure_reason,"ordinary fragmented post-request answer stays valid");
        require(!second.close_stdin&&!second.complete&&second.stdin_bytes,"Qwen asks for conditional close before EOF");
        const auto closed=inspect(second.protocol_state,wire::close_response("s"));
        require(closed.close_stdin&&!closed.complete&&!closed.failure_reason,"acknowledged native close requests EOF exactly once");
        const auto advisory=inspect(closed.protocol_state,commands);
        require(!advisory.complete&&!advisory.close_stdin&&!advisory.stdin_bytes&&!advisory.failure_reason,"Qwen drain remains live without repeating EOF");
        const auto settled=inspect(advisory.protocol_state,"",true);
        require(settled.complete&&!settled.failure_reason&&settled.result_bytes=="ANSWER","Qwen requires clean final native exit");
        require(inspect(closed.protocol_state,"",true,7).failure_reason.has_value(),"failed exit overrides retained result");
        require(inspect(closed.protocol_state,model_text).failure_reason.has_value(),"late model output is not a drain advisory");
    }
    const auto close_reply = wire::close_response("s");
    const auto finished_bytes = model_text + wire::active_work(1, "s") + completion;
    const auto closing = inspect(prompt.protocol_state, finished_bytes);
    require(closing.stdin_bytes && !closing.failure_reason, "close causality fixture issues one real close request");
    for (std::size_t split = 1; split < close_reply.size(); ++split) {
        const auto premature = inspect(prompt.protocol_state, finished_bytes + close_reply.substr(0, split));
        require(!premature.failure_reason && !premature.stdin_bytes && !premature.close_stdin,
            "an unclassified pre-request close prefix prevents releasing the close request");
        require(inspect(premature.protocol_state, close_reply.substr(split)).failure_reason == "out_of_order_acp_response",
            "every premature close suffix retains its pre-request provenance");
        const auto prefix = inspect(closing.protocol_state, close_reply.substr(0, split));
        require(!prefix.failure_reason && !prefix.close_stdin && !prefix.stdin_bytes,
            "a partial acknowledgment never requests EOF");
        const auto suffix = inspect(prefix.protocol_state, close_reply.substr(split));
        require(!suffix.failure_reason && suffix.close_stdin && !suffix.complete,
            "every valid split acknowledges the issued close exactly once");
        require(inspect(suffix.protocol_state, "", true).result_bytes == "ANSWER",
            "fragmented close preserves the complete authenticated answer");
    }
    std::cout<<"qwen_causal_assertions="<<assertions<<" failures="<<failures<<'\n';
    return failures ? 1 : 0;
}
