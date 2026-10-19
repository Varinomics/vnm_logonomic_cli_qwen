#include "qwen_session_wake.h"

#include "qwen_json.h"
#include "qwen_peer.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <optional>
#include <stdexcept>

namespace json_util = logonomic::cli_addon::json;

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;

#ifdef __linux__
std::optional<Peer_target> bound_peer(const addon::Session_wake_target& target)
{
    if (!json_util::exact_keys(target.binding, {"profile_path", "account_key", "launch_token"}) ||
        !target.launch_environment.bootstrap ||
        !addon::path_from_utf8(target.launch_environment.bootstrap->resolved_home).is_absolute())
    {
        return std::nullopt;
    }
    const auto profile = profile_from_environment(target.launch_environment);
    if (path_string(profile.home) != json_util::text(target.binding, "profile_path") ||
        account_key(profile.home) != json_util::text(target.binding, "account_key"))
    {
        return std::nullopt;
    }
    return find_peer(profile.home, target.provider_session_id, json_util::text(target.binding, "launch_token"));
}
#endif

addon::Session_wake_notify_result notify(const addon::Session_wake_notify_request& request,
    addon::Invocation_context& context)
{
    using Disposition = addon::Session_wake_disposition;
#ifndef __linux__
    (void)request;
    (void)context;
    return {Disposition::UNAVAILABLE, "qwen_peer_platform_unavailable", std::nullopt};
#else
    const auto target = bound_peer(request.target);
    if (!target) {
        return {Disposition::UNAVAILABLE, "qwen_peer_session_unavailable", std::nullopt};
    }
    // The native protocol permits 64 hexadecimal message-id characters. Bind
    // its deduplication identity to both the host notification and exact session.
    const auto message_id = addon::to_hex(addon::Sha256::digest(
        request.target.provider_session_id + ":" + request.notification_id));
    const auto result = send_peer(*target, message_id, request.instruction,
        [&context] { return context.cancel_requested(); });
    if (result.status == "delivered") {
        return {Disposition::ACCEPTED, {}, message_id};
    }
    if (result.status == "held") {
        // A held message is retained by Qwen and may be released later. It
        // must suppress host retries just like an accepted model-input queue.
        return {Disposition::ACCEPTED, {}, "held:" + message_id};
    }
    if (result.status == "unknown" && result.written) {
        return {Disposition::UNCERTAIN, "qwen_peer_receipt_unavailable", std::nullopt};
    }
    return {Disposition::UNAVAILABLE, "qwen_peer_" + result.status, std::nullopt};
#endif
}

} // namespace

addon::Session_wake_preparation prepare_session_wake(const addon::Session_launch_context& context)
{
#ifdef __linux__
    const auto profile = profile_for_context(context);
    return {true, json_util::object({{"profile_path", Json(path_string(profile.home))},
        {"account_key", Json(account_key(profile.home))}, {"launch_token", Json(addon::random_nonce_hex())}}), {}};
#else
    (void)context;
    return {false, Json::object(), "qwen_peer_platform_unavailable"};
#endif
}

void handle_session_wake(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_session_wake_inspect_operation) {
        const auto request = addon::decode_session_wake_inspect_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Session_wake_readiness result;
#ifdef __linux__
        const auto target = bound_peer(request->target);
        result.available = target && peer_reachable(*target);
        result.reason = result.available ? "" : "qwen_peer_session_unavailable";
#else
        result.reason = "qwen_peer_platform_unavailable";
#endif
        context.succeed(addon::encode_session_wake_readiness(result));
        return;
    }
    if (context.request().operation == addon::k_session_wake_notify_operation) {
        const auto request = addon::decode_session_wake_notify_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_session_wake_notify_result(notify(*request, context)));
        return;
    }
    context.reject_unknown_operation();
}

} // namespace qwen
