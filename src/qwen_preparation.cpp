#include "qwen_preparation.h"
#include "qwen_acp.h"
#include "qwen_config.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_peer.h"
#include "qwen_profile.h"
#include "qwen_session_wake.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

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

bool verbatim_initial_prompt(std::string_view text)
{
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    if (addon::trim(text) != text || text.starts_with(bom) || text.ends_with(bom) ||
        text.starts_with('/') || text == "exit" || text == "quit" || text == ":q" ||
        text == ":q!" || text == ":wq" || text == ":wq!")
    {
        return false;
    }
    if (text.starts_with("?btw")) {
        const auto remainder = text.substr(4);
        if (remainder.empty() || addon::trim(remainder) != remainder || remainder.starts_with(bom)) {
            return false;
        }
    }
    // yargs strips matching surrounding quotes in the --option=value form.
    if ((text.front() == '\'' || text.front() == '"') && text.back() == text.front()) {
        return false;
    }
    // Native Qwen expands whitespace-prefixed @ references before submitting.
    for (auto at = text.find('@'); at != std::string_view::npos; at = text.find('@', at + 1)) {
        const auto before = text.substr(0, at);
        if (before.empty() || addon::trim(before) != before || before.ends_with(bom)) {
            return false;
        }
    }
    return true;
}

bool initial_prompt_options(const std::vector<std::string>& argv)
{
    if (!plain_session_options(argv)) {
        return false;
    }
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& option = argv[index];
        if (option == "--model" || option == "-m" || option == "--approval-mode" ||
            option == "--openai-api-key" || option == "--openai-base-url")
        {
            // yargs treats a leading dash as another option, even when a
            // preceding single-value option has not received its operand.
            if (argv[++index].starts_with('-')) {
                return false;
            }
        }
    }
    return true;
}

#ifdef _WIN32
bool powershell_initial_argument(std::string_view argument)
{
    for (const std::string_view quote : {"\"", "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\xE2\x80\x9B"}) {
        if (argument.find(quote) != std::string_view::npos) {
            return false;
        }
    }
    return !argument.ends_with('\\');
}
#endif

std::optional<std::vector<std::string>> initial_prompt_command(
    const addon::Prepare_request& request,
    const Environment& environment,
    addon::Invocation_context& invocation)
{
    const auto& prompt = *request.initial_prompt;
    const auto brief = addon::path_from_utf8(prompt.referenced_file);
    const auto directory = brief.parent_path();
    const auto directory_text = path_string(directory);
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    if (!verbatim_initial_prompt(prompt.text) || directory == directory.root_path() ||
        brief.lexically_normal() != brief || directory_text.find(',') != std::string::npos ||
        addon::trim(directory_text) != directory_text || directory_text.starts_with(bom) ||
        directory_text.ends_with(bom))
    {
        return std::nullopt;
    }
    const std::initializer_list<std::string_view> required_options{"--prompt-interactive", "--include-directories"};
    const std::vector<std::string> additions{
        "--include-directories", directory_text, "--prompt-interactive=" + prompt.text};
#ifdef _WIN32
    if (request.launcher.runner_key == addon::k_runner_powershell) {
        const auto selected = inspect_powershell_launcher(request.launcher, request.launcher_argv,
            environment, request.working_directory, invocation);
        if (!selected || !initial_prompt_options(selected->option_values) ||
            !powershell_initial_argument(prompt.text) || !powershell_initial_argument(additions[1]))
        {
            return std::nullopt;
        }
        auto checked = selected->option_values;
        checked.insert(checked.end(), additions.begin(), additions.end());
        if (!platform_command(checked, environment, request.working_directory) ||
            !selected_program_supports(selected->program, environment, request.working_directory,
                required_options, invocation))
        {
            return std::nullopt;
        }
        return powershell_session_command(*selected, additions);
    }
#endif
    auto command = selected_command(request.launcher, request.launcher_argv,
        environment, request.working_directory);
    if (!command || !initial_prompt_options(*command)) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto extension = lower(addon::path_from_utf8(command->front()).extension().string());
    if ((extension == ".ps1" || extension == ".cmd" || extension == ".bat") &&
        (!powershell_initial_argument(prompt.text) || !powershell_initial_argument(additions[1])))
    {
        return std::nullopt;
    }
#endif
    if (!selected_program_supports(command->front(), environment, request.working_directory,
            required_options, invocation))
    {
        return std::nullopt;
    }
    command->insert(command->end(), additions.begin(), additions.end());
    return platform_command(*command, environment, request.working_directory);
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
    if (request->initial_prompt) {
        patch.argv = initial_prompt_command(*request, environment, invocation);
        if (!patch.argv) {
            invocation.fail(invocation.cancel_requested()
                    ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
                invocation.cancel_requested() ? "cancelled" : addon::reason::k_initial_prompt_unavailable);
            return;
        }
        patch.initial_prompt = addon::Initial_prompt_acknowledgment{
            addon::initial_prompt_text_digest_hex(request->initial_prompt->text)};
    }
    const bool direct = request->launcher.runner_key == "direct";
    bool admitted = false;
    if (request->initial_prompt) {
        admitted = true;
    }
    else
    if (direct) {
        const auto selected = selected_command(request->launcher, request->launcher_argv,
            environment, request->working_directory);
        admitted = selected.has_value();
        if (selected) {
            patch.argv = platform_command(*selected, environment, request->working_directory);
            if (!patch.argv) {
                invocation.fail(addon::Result_status::UNAVAILABLE, "launch_program_unavailable");
                return;
            }
        }
    }
#ifdef _WIN32
    else
    if (request->launcher.runner_key == addon::k_runner_powershell) {
        const auto selected = inspect_powershell_launcher(request->launcher, request->launcher_argv,
            environment, request->working_directory, invocation);
        admitted = selected.has_value();
    }
#endif
    else {
        admitted = is_qwen_invocation(request->launcher_argv);
    }
    if (!admitted) {
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
    if (request->session_wake) {
        patch.session_wake = prepare_session_wake(context);
        if (patch.session_wake->available) {
            patch.set.emplace_back(k_wake_binding_environment, json_util::text(patch.session_wake->binding, "launch_token"));
        }
    }
    // The host's bootstrap owns the effective home. This requests a pure
    // snapshot transform against that same declared target and a host CAS.
    patch.disposition = addon::Launch_disposition{true, true, true};
    patch.apply_managed_configuration = true;
    if (request->initial_prompt && !addon::validate_launch_patch(patch, {}, {}, &failure)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
        return;
    }
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
    return result;
}

void prepare_structured(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_structured_prepare_request(invocation.request().payload, &failure);
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
    result.process.stdin_payload = initial_acp_input(*request);
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    result.inspection = {true, initial_acp_state(request->prompt), true};
    invocation.succeed(addon::encode_structured_prepare_result(result));
}

void prepare_delegated(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(invocation.request().payload, &failure);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!request->control.connection) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
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
    const auto receipts = task_server_receipts(*request);
    addon::Delegated_prepare_result result;
    result.process = *process;
    result.process.stdin_payload = initial_acp_input(*request, receipts);
    // Qwen otherwise discovers MCP servers after it answers session/new, and
    // its ACP prompt does not wait for them. With blocking discovery, the
    // session reply follows every Task server's connection or failure.
    set_environment(result.process.environment, "QWEN_CODE_LEGACY_MCP_BLOCKING", "1");
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    result.inspection = {true, initial_acp_state(request->prompt, receipts), true};
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    for (const auto& receipt : receipts) {
        result.acknowledgment.server_ids.push_back(receipt.server_id);
        result.private_artifacts.push_back({receipt.parent_artifact, receipt.nonce});
    }
    invocation.succeed(addon::encode_delegated_prepare_result(result));
}

void prepare_resume(addon::Invocation_context& invocation)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(invocation.request().payload, &failure, 2);
    if (!request) {
        invocation.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto session = json_util::text(request->resume_record, "provider_session_id");
    if (!bootstrap_available(request->context.launch_environment, invocation)) {
        return;
    }
    static const std::regex uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    if (!json_util::exact_keys(request->resume_record, {"provider_session_id"}) || !std::regex_match(session, uuid)) {
        invocation.fail(addon::Result_status::UNAVAILABLE, "resume_selector_unavailable");
        return;
    }
    const auto profile = profile_for_context(request->context);
#ifdef _WIN32
    std::optional<Powershell_selection> powershell;
    if (request->context.launcher.runner_key == addon::k_runner_powershell) {
        powershell = inspect_powershell_launcher(request->context.launcher, request->context.launcher_argv,
            profile.environment, request->context.working_directory, invocation);
        if (!powershell) {
            invocation.fail(addon::Result_status::UNAVAILABLE, "resume_selector_unavailable");
            return;
        }
    }
    else
#endif
    if (request->context.launcher.runner_key != addon::k_runner_direct ||
        !plain_session_options(request->context.launcher_argv))
    {
        invocation.fail(addon::Result_status::UNAVAILABLE, "resume_selector_unavailable");
        return;
    }
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
    std::optional<std::vector<std::string>> command;
#ifdef _WIN32
    if (powershell) {
        if (selected_program_supports(powershell->program, profile.environment,
            request->context.working_directory, {"--resume"}, invocation))
        {
            command = powershell_session_command(*powershell, {"--resume", session});
        }
    }
    else
#endif
    {
        command = verified_command(request->context.launcher, request->context.launcher_argv,
            profile.environment, request->context.working_directory, "--resume", invocation);
        if (command) {
            command->insert(command->end(), {"--resume", session});
            command = platform_command(*command, profile.environment, request->context.working_directory);
        }
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
    if (request->context.session_wake) {
        result.patch.session_wake = prepare_session_wake(request->context);
        if (result.patch.session_wake->available) {
            result.patch.set.emplace_back(k_wake_binding_environment, json_util::text(result.patch.session_wake->binding, "launch_token"));
        }
    }
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
