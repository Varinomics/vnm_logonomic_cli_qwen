#include "qwen_acp.h"
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
const std::string initialize=R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"agentInfo":{"name":"qwen-code","title":"Qwen Code","version":"fixture"},"authMethods":[],"agentCapabilities":{}}})" "\n";
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
    auto state = *json_util::parse(provider::initial_acp_state(request.prompt, provider::task_server_receipts(request)));
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
    for (const auto& name : {own, std::string("run_shell_command"), own + "_other",
            std::string("mcp__user_server__submit")}) {
        const auto result = inspect(pending, permission(name) + "{");
        const auto expected = name == own ? "proceed_once" : "cancel";
        require(!result.failure_reason && result.stdin_bytes &&
            result.stdin_bytes->find(std::string("\"optionId\":\"") + expected + "\"") != std::string::npos,
            "permission replies immediately: own Task tool once, everything else rejected once");
        const auto next = inspect(result.protocol_state, "");
        require(!next.stdin_bytes, "a permission reply is not replayed across a partial later record");
    }
    require(inspect(pending, permission(own, "foreign")).failure_reason.has_value(),
        "a foreign session cannot obtain an approval");
    require(inspect(pending, permission(own, "s", false)).failure_reason.has_value(),
        "missing plain allow-once refuses instead of selecting a persistent or mode-changing approval");
}

int main() {
    launcher_permissions();
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
        const auto second=inspect(first.protocol_state,model_text.substr(split)+completion);
        require(!first.failure_reason&&!second.failure_reason,"ordinary fragmented post-request answer stays valid");
        require(second.close_stdin&&!second.complete,"Qwen asks for EOF rather than early success");
        const auto advisory=inspect(second.protocol_state,commands);
        require(!advisory.complete&&!advisory.close_stdin&&!advisory.stdin_bytes&&!advisory.failure_reason,"Qwen drain remains live without repeating EOF");
        const auto settled=inspect(advisory.protocol_state,"",true);
        require(settled.complete&&!settled.failure_reason&&settled.result_bytes=="ANSWER","Qwen requires clean final native exit");
        require(inspect(second.protocol_state,"",true,7).failure_reason.has_value(),"failed exit overrides retained result");
        require(inspect(second.protocol_state,model_text).failure_reason.has_value(),"late model output is not a drain advisory");
    }
    std::cout<<"qwen_causal_assertions="<<assertions<<" failures="<<failures<<'\n';
    return failures ? 1 : 0;
}
