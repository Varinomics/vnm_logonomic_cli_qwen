#include "qwen_preparation.h"
#include "qwen_acp.h"
#include "qwen_config.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <utility>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* k_bridge = "helpers/qwen_launch_bridge.exe";
#else
constexpr const char* k_bridge = "helpers/qwen_launch_bridge";
#endif

bool bootstrap_available(const std::optional<addon::Launch_environment>& environment,
    addon::Invocation_context& invocation)
{
    if (environment && environment->bootstrap) {
        return true;
    }
    invocation.fail(addon::Result_status::UNAVAILABLE, "environment_bootstrap_unavailable");
    return false;
}

bool plain_session_options(const std::vector<std::string>& argv)
{
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& option = argv[index];
        if (option == "--debug" || option == "-d" || option == "--yolo" || option == "-y") {
            continue;
        }
        if (option == "--model" || option == "-m" || option == "--approval-mode" ||
            option == "--openai-api-key" || option == "--openai-base-url")
        {
            if (++index < argv.size() && !argv[index].empty()) {
                continue;
            }
        }
        return false;
    }
    return true;
}

void prepare_launch(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(invocation.request().payload, &failure, 3);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Launch_patch patch;
    if (!bootstrap_available(request->launch_environment, invocation)) {
        return;
    }
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    const auto environment = environment_pairs(request->launch_environment->effective_environment);
    const bool direct = request->launcher.runner_key == "direct";
    if ((direct && !selected_command(request->launcher, request->launcher_argv, environment, request->working_directory)) ||
        (!direct && !is_qwen_invocation(request->launcher_argv)))
    {
        patch.disposition = addon::Launch_disposition{false, false, false};
        invocation.succeed(addon::encode_launch_patch(patch, 3));
        return;
    }
    addon::Session_launch_context context;
    context.launcher = request->launcher;
    context.launcher_argv = request->launcher_argv;
    context.working_directory = request->working_directory;
    context.account_context = request->account_context;
    context.launch_environment = request->launch_environment;
    (void)profile_for_context(context);
    // The host's bootstrap owns the effective home. This requests a pure
    // snapshot transform against that same declared target and a host CAS.
    patch.disposition = addon::Launch_disposition{true, true, true};
    patch.apply_managed_configuration = true;
    invocation.succeed(addon::encode_launch_patch(patch, 3));
}

template<class Request>
std::optional<addon::Process_proposal> acp_process(
    const Request& request, addon::Invocation_context& invocation)
{
    if (request.purpose != addon::Structured_purpose::IMPLEMENTATION || request.prompt.empty() ||
        !addon::is_valid_utf8(request.prompt) || !plain_session_options(request.context.launcher_argv))
    {
        return std::nullopt;
    }
    const auto profile = profile_for_context(request.context);
    auto command = verified_command(request.context.launcher, request.context.launcher_argv,
        profile.environment, request.context.working_directory, "--acp", invocation);
    if (!command) {
        return std::nullopt;
    }
    command->push_back("--acp");
    command = platform_command(*command, profile.environment, request.context.working_directory);
    if (!command) {
        return std::nullopt;
    }
    addon::Process_proposal result;
    result.executable = k_bridge;
    result.argv = std::move(*command);
    result.environment = profile.environment;
    result.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.stdin_payload = initial_acp_input(request);
    return result;
}

void prepare_structured(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_structured_prepare_request(invocation.request().payload, &failure, 3);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!bootstrap_available(request->context.launch_environment, invocation)) {
        return;
    }
    const auto process = acp_process(*request, invocation);
    if (!process) {
        invocation.fail(invocation.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            invocation.cancel_requested() ? "cancelled" : "structured_launcher_unavailable");
        return;
    }
    addon::Structured_prepare_result result;
    result.process = *process;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    result.inspection = {true, initial_acp_state(request->prompt), true};
    invocation.succeed(addon::encode_structured_prepare_result(result, 3));
}

void prepare_delegated(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(invocation.request().payload, &failure, 3);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!bootstrap_available(request->context.launch_environment, invocation)) {
        return;
    }
    const auto process = acp_process(*request, invocation);
    if (!process) {
        invocation.fail(invocation.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            invocation.cancel_requested() ? "cancelled" : "delegated_launcher_unavailable");
        return;
    }
    addon::Delegated_prepare_result result;
    result.process = *process;
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    result.inspection = {true, initial_acp_state(request->prompt), true};
    for (const auto& server : request->control.servers) {
        result.acknowledgment.server_ids.push_back(server.server_id);
    }
    invocation.succeed(addon::encode_delegated_prepare_result(result, 3));
}

void prepare_resume(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(invocation.request().payload, &failure, 2);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto session = text(request->resume_record, "provider_session_id");
    if (!bootstrap_available(request->context.launch_environment, invocation)) {
        return;
    }
    static const std::regex uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    if (!exact_keys(request->resume_record, {"provider_session_id"}) || !std::regex_match(session, uuid) ||
        !plain_session_options(request->context.launcher_argv))
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "resume_selector_unavailable");
        return;
    }
    const auto profile = profile_for_context(request->context);
    const auto config = profile.home / "settings.json";
    std::error_code error;
    if (fs::file_size(config, error) > 768 * 1024 || error) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "managed_hooks_unavailable");
        return;
    }
    std::ifstream input(config, std::ios::binary);
    const std::string settings{std::istreambuf_iterator<char>(input), {}};
    if (!input || !has_managed_hooks(settings)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "managed_hooks_unavailable");
        return;
    }
    auto command = verified_command(request->context.launcher, request->context.launcher_argv,
        profile.environment, request->context.working_directory, "--resume", invocation);
    if (command) {
        command->insert(command->end(), {"--resume", session});
        command = platform_command(*command, profile.environment, request->context.working_directory);
    }
    if (!command) {
        invocation.fail(invocation.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            invocation.cancel_requested() ? "cancelled" : "resume_launcher_unavailable");
        return;
    }
    addon::Resume_prepare_result result;
    result.process.executable = k_bridge;
    result.process.argv = std::move(*command);
    result.patch.set = profile.environment;
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    invocation.succeed(addon::encode_resume_prepare_result(result, 2));
}

} // namespace

void handle_preparation(addon::Invocation_context& invocation)
{
    const auto& request = invocation.request();
    if (request.capability.capability_id == "interactive-resume" && request.operation == "prepare_resume") {
        prepare_resume(invocation);
    }
    else
    if (request.operation != "prepare") {
        invocation.reject_unknown_operation();
    }
    else
    if (request.capability.capability_id == "launch-preparation") {
        prepare_launch(invocation);
    }
    else
    if (request.capability.capability_id == "structured-session") {
        prepare_structured(invocation);
    }
    else
    if (request.capability.capability_id == "delegated-session") {
        prepare_delegated(invocation);
    }
    else {
        invocation.reject_unknown_operation();
    }
}

} // namespace qwen
