#include "qwen_accounts.h"
#include "qwen_acp.h"
#include "qwen_config.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_lifecycle.h"
#include "qwen_preparation.h"
#include "qwen_usage.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == "discover") {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto candidate = qwen::discover_launcher(*request);
        context.succeed(addon::encode_candidates(candidate
            ? std::vector<addon::Launcher_candidate>{*candidate} : std::vector<addon::Launcher_candidate>{}));
    }
    else
    if (context.request().operation == "recognize") {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Launcher_recognition result;
        result.recognized = qwen::recognize_launcher(request->launcher.script_body);
        if (result.recognized) {
            result.display_name = "Qwen Code";
        }
        context.succeed(addon::encode_recognition(result));
    }
    else {
        context.reject_unknown_operation();
    }
}

void transform(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation != addon::k_managed_configuration_transform_operation) {
        context.reject_unknown_operation();
        return;
    }
    const auto request = addon::decode_managed_configuration_transform_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto content = qwen::updated_config(request->content);
    if (content.size() > addon::k_max_managed_configuration_bytes) {
        context.fail(addon::Result_status::UNAVAILABLE, "settings_size_limit");
        return;
    }
    context.succeed(addon::encode_managed_configuration_transform_result({request->grant, content}));
}

void invoke(addon::Invocation_context& context)
{
    try {
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        const auto& request = context.request();
        const auto& capability = request.capability.capability_id;
        if (capability == "launcher-discovery") {
            discover(context);
        }
        else
        if (capability == "managed-configuration") {
            transform(context);
        }
        else
        if (capability == "accounts") {
            qwen::handle_accounts(context);
        }
        else
        if (capability == "profile-storage" && request.operation == "acknowledge") {
            std::string error;
            const auto* value = request.payload.find("profile_storage");
            const auto storage = value ? addon::decode_profile_storage_context(*value, &error) : std::nullopt;
            if (!qwen::exact_keys(request.payload, {"profile_storage"}) || !storage) {
                context.fail(addon::Result_status::FAILED, "profile_storage_mismatch");
                return;
            }
            context.succeed(qwen::object({{"profile_storage", addon::profile_storage_member(*storage)}}));
        }
        else
        if (capability == "usage-scan") {
            qwen::handle_usage(context);
        }
        else
        if (capability == addon::k_attention_kind_capability_id && request.operation == addon::k_classify_event_operation) {
            addon::Contract_failure failure;
            const auto decoded = addon::decode_attention_classification_request(request.payload, &failure);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            context.succeed(addon::encode_attention_classification_result(qwen::classify_attention(*decoded)));
        }
        else
        if (capability == "helper-ingress" && request.operation == addon::k_classify_event_operation) {
            addon::Contract_failure failure;
            const auto decoded = addon::decode_classify_event_request(request.payload, &failure);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            context.succeed(addon::encode_classify_event_result(qwen::classify_event(*decoded)));
        }
        else
        if (capability == "structured-session" && request.operation == "inspect") {
            addon::Contract_failure failure;
            const auto decoded = addon::decode_structured_inspect_request(request.payload, &failure, 3);
            if (!decoded) {
                context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
                return;
            }
            if (decoded->purpose != addon::Structured_purpose::IMPLEMENTATION) {
                context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
                return;
            }
            context.succeed(addon::encode_structured_inspect_result(qwen::inspect_acp(decoded->inspection), 3));
        }
        else {
            qwen::handle_preparation(context);
        }
    }
    catch (const std::exception&) {
        // Vendor errors can contain prompts, credentials or settings bytes.
        context.fail(addon::Result_status::FAILED, "qwen_operation_failed",
            "Qwen Code could not complete the requested operation.");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name = "qwen-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1},
        {"accounts", 2}, {"usage-scan", 2}, {"usage-scan", 3},
        {"profile-storage", 1},
        {"launch-preparation", 3}, {"managed-configuration", 2}, {"helper-ingress", 2},
        {"lifecycle-events", 1}, {"coordination-delivery", 1},
        {"model-context-delivery", 1},
        {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        {"environment-bootstrap", 1},
        {"structured-session", 3}, {"delegated-session", 3}, {"interactive-resume", 2},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
