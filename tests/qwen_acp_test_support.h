#pragma once

#include "qwen_json.h"

#include <logonomic_cli_addon/package_source.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace qwen::acp_test {

namespace addon = logonomic::cli_addon;
namespace json_util = logonomic::cli_addon::json;

// Wire fixtures follow Qwen Code v0.27.4 (f35dde84157f992bc324c0a3f59b1119344a683d):
// packages/acp-bridge/src/bridgeTypes.ts and
// packages/cli/src/acp-integration/{acpAgent.ts,active-work-reporter.ts,session/Session.ts}.
// They model the native contract, including complete snapshots and the
// conditional close response, independently of the addon's stored state.
inline std::string record(Json value)
{
    return addon::serialize_json(value) + '\n';
}

inline Json active_work_capability()
{
    auto categories = Json::array();
    for (const auto* category : {"agent", "notification", "shell", "session", "workflow", "tool"}) {
        categories.push_back(Json(category));
    }
    return json_util::object({{"v", Json(1)}, {"intervalMs", Json(5000)}, {"categories", std::move(categories)}});
}

inline std::string initialize_response(Json capability = active_work_capability())
{
    auto result = json_util::object({
        {"protocolVersion", Json(1)},
        {"agentInfo", json_util::object({{"name", Json("qwen-code")}, {"title", Json("Qwen Code")}, {"version", Json("0.27.4")}})},
        {"authMethods", Json::array()}, {"agentCapabilities", Json::object()},
        {"_meta", json_util::object({{"qwen.daemon.activeWorkHeartbeat", std::move(capability)}})}});
    return record(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(1)}, {"result", std::move(result)}}));
}

inline Json hold(std::string category, std::string id)
{
    return json_util::object({{"category", Json(std::move(category))}, {"id", Json(std::move(id))}});
}

inline Json one_hold(std::string category = "agent", std::string id = "child-1")
{
    auto holds = Json::array();
    holds.push_back(hold(std::move(category), std::move(id)));
    return holds;
}

inline Json active_work_session(std::string_view session, Json holds = Json::array())
{
    return json_util::object({{"sessionId", Json(std::string(session))}, {"holds", std::move(holds)}});
}

inline std::string active_work_snapshot(std::int64_t sequence, Json sessions)
{
    return record(json_util::object({{"jsonrpc", Json("2.0")},
        {"method", Json("qwen/notify/channel/active-work")},
        {"params", json_util::object({{"v", Json(1)}, {"seq", Json(sequence)}, {"sessions", std::move(sessions)}})}}));
}

inline std::string active_work(
    std::int64_t sequence, std::string_view session = "session-alpha", Json holds = Json::array())
{
    auto sessions = Json::array();
    sessions.push_back(active_work_session(session, std::move(holds)));
    return active_work_snapshot(sequence, std::move(sessions));
}

inline std::string close_response(std::string_view session = "session-alpha", std::int64_t id = 6,
    bool closed = true, Json holds = Json::array())
{
    auto result = json_util::object({{"sessionId", Json(std::string(session))}, {"closed", Json(closed)}});
    if (!closed) {
        result.set("holds", std::move(holds));
    }
    return record(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)}, {"result", std::move(result)}}));
}

inline std::string message(std::string_view text, Json meta = Json::object(),
    std::string_view session = "session-alpha")
{
    return record(json_util::object({{"jsonrpc", Json("2.0")}, {"method", Json("session/update")},
        {"params", json_util::object({{"sessionId", Json(std::string(session))},
            {"update", json_util::object({{"sessionUpdate", Json("agent_message_chunk")},
                {"content", json_util::object({{"type", Json("text")}, {"text", Json(std::string(text))}})},
                {"_meta", std::move(meta)}})}})}}));
}

inline std::string prompt_response(std::int64_t id = 4, std::string_view reason = "end_turn")
{
    return record(json_util::object({{"jsonrpc", Json("2.0")}, {"id", Json(id)},
        {"result", json_util::object({{"stopReason", Json(std::string(reason))}})}}));
}

} // namespace qwen::acp_test
