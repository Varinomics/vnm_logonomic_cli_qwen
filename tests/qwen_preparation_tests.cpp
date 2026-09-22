#include "qwen_preparation.h"
#include "qwen_accounts.h"
#include "qwen_profile.h"
#include "qwen_config.h"
#include "qwen_launcher.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return m_cancelled; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { m_result = std::move(result); }
    void fail(addon::Result_status status, std::string reason, std::string) override { m_failure_status = status; throw std::runtime_error(reason); }
    bool finished() const override { return m_result.has_value(); }

    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
    std::optional<addon::Result_status> m_failure_status;
    bool m_cancelled = false;
};

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

void required_control_is_explicitly_unavailable()
{
    // Regression: invoke the real preparation owner with a decoder-valid envelope.
    // A missing native executable is deliberate: the action-admission boundary
    // must answer first, without running a native probe or creating a proposal.
    const auto workspace = addon::path_to_utf8(std::filesystem::current_path());
    const auto home = addon::path_to_utf8(std::filesystem::current_path() / ".qwen-profile");
    const auto program = addon::path_to_utf8(std::filesystem::current_path() / "absent-required-control-native");
    addon::Delegated_prepare_request request;
    request.context.launcher = {"qwen", addon::k_runner_direct, program};
    request.context.launcher_argv = {program};
    request.context.working_directory = workspace;
    request.context.account_context = addon::Account_context{2,
        qwen::account_key(addon::path_from_utf8(home)), home};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.context.launch_environment = environment;
    request.prompt = "A task that must not be released without its required control action.";
    request.control.task_id = "required-control-regression";
    request.control.private_artifact_root = workspace;
    request.control.servers.push_back({"control", program, {"task_status"},
        {{"LOGONOMIC_TASK_ID", request.control.task_id}}});
    addon::Contract_failure failure;
    const auto payload = addon::encode_delegated_prepare_request(request, 3);
    require(addon::decode_delegated_prepare_request(payload, &failure, 3).has_value(),
        "containment fixture must satisfy the actual public decoder");
    Context context;
    context.m_request = {"required-control", {"delegated-session", 3}, "prepare", payload};
    bool refused = false;
    try { qwen::handle_preparation(context); }
    catch (const std::runtime_error& error) {
        refused = std::string(error.what()) == "required_control_admission_unavailable";
    }
    require(refused && context.m_failure_status == addon::Result_status::UNAVAILABLE && !context.m_result,
        "unsupported required actions must produce no process proposal or task bytes");
    Context cancelled;
    cancelled.m_request = context.m_request;
    cancelled.m_cancelled = true;
    bool cancellation_preserved = false;
    try { qwen::handle_preparation(cancelled); }
    catch (const std::runtime_error& error) {
        cancellation_preserved = std::string(error.what()) == "cancelled";
    }
    require(cancellation_preserved && cancelled.m_failure_status == addon::Result_status::CANCELLED && !cancelled.m_result,
        "the fail-closed boundary must not relabel an already cancelled request");
    // The SDK accepts empty action lists, but the host requires nonempty ones.
    // Decoder acceptance here cannot establish a working host delegation path.
    request.control.servers.front().actions.clear();
    require(addon::decode_delegated_prepare_request(
        addon::encode_delegated_prepare_request(request, 3), &failure, 3).has_value(),
        "empty-action control servers remain decoder-valid, not host-valid");
}

void launch_disposition()
{
    // Oracle: the launch-preparation@3 decoder requires explicit feature
    // admission. Exercise the actual provider producer without running a CLI.
    addon::Prepare_request request;
    request.working_directory = addon::path_to_utf8(std::filesystem::current_path());
    request.launcher = {"qwen", "bash", ""};
    request.launcher_argv = {"bash", "-c", "qwen"};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    const auto home = addon::path_to_utf8(std::filesystem::current_path() / ".qwen-profile");
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.launch_environment = environment;
    request.account_context = addon::Account_context{2, qwen::account_key(addon::path_from_utf8(home)), home};
    for (const bool prepared : {true, false}) {
        if (!prepared) {
            request.launcher.script_body = "echo unrelated";
            request.launcher_argv = {"bash", "-c", "echo unrelated"};
        }
        Context context;
        context.m_request.capability = {"launch-preparation", 3};
        context.m_request.operation = "prepare";
        context.m_request.payload = addon::encode_prepare_request(request, 3);
        qwen::handle_preparation(context);
        require(context.m_result.has_value(), "provider must return a terminal preparation");
        addon::Contract_failure failure;
        const auto patch = addon::decode_launch_patch(*context.m_result, &failure, 3);
        require(patch && patch->disposition && patch->disposition->prepared == prepared &&
            patch->disposition->lifecycle_events == prepared &&
            patch->disposition->coordination_delivery == prepared &&
            patch->apply_managed_configuration == prepared,
            "actual provider success must round-trip explicit admission and managed intent");
        require(patch->account_context && patch->account_context->adapter_account_key == request.account_context->adapter_account_key &&
            patch->account_context->profile_path == request.account_context->profile_path, "preparation retains the admitted account");
        require(addon::validate_launch_patch(*patch, {}, {}, &failure), "returned patch must satisfy host policy");
        if (!prepared) require(patch->set.empty() && patch->unset.empty() && !patch->argv,
            "unmanaged preparation must preserve the native launch");
    }
}

void resume_policy(const char* executable)
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("qwen-resume-" + addon::random_nonce_hex());
    fs::create_directories(root / "selected");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    const auto home = addon::path_to_utf8(root / "selected");
#ifdef _WIN32
    const auto fake = root / "qwen.exe";
#else
    const auto fake = root / "qwen";
#endif
    fs::copy_file(fs::absolute(executable), fake);
    {
        std::ofstream config(root / "selected" / "settings.json", std::ios::binary);
        config << qwen::updated_config("{}");
        require(bool(config), "resume fixture configuration must be written");
    }
    addon::Resume_prepare_request request;
    request.context.launcher.runner_key  = addon::k_runner_direct;
    request.context.launcher.script_body = '"' + addon::path_to_utf8(fake) + '"';
    request.context.launcher_argv        = {addon::path_to_utf8(fake)};
    request.context.working_directory    = addon::path_to_utf8(root);
    request.context.account_context = addon::Account_context{2, qwen::account_key(root / "selected"), home};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = "linux-x86_64";
#endif
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.effective_environment.set("OPENAI_API_KEY", addon::Json_value("frozen-fixture-key"));
    for (const auto* name : {"SystemRoot", "PATH"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.context.launch_environment = environment;
    request.resume_record.set("provider_session_id", addon::Json_value("019933a1-6e43-7102-a520-b3c8294cc763"));
    Context context;
    context.m_request = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(request, 2)};
    qwen::handle_preparation(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_resume_prepare_result(*context.m_result, &failure, 2) : std::nullopt;
    require(result && addon::validate_resume_process_proposal(result->process, &failure) &&
        addon::validate_launch_patch(result->patch, {}, {}, &failure), "resume producer must satisfy terminal policy");
    require(result->patch.set == qwen::environment_pairs(environment.effective_environment),
        "resume patch must retain the frozen profile and authentication environment");
    require(result->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
        addon::validate_resume_acknowledgment(request.resume_record, result->acknowledgment, &failure),
        "resume must preserve the selected native session identity");
}

} // namespace

void account_names()
{
    // Oracle: accounts@1 names an account with a filesystem profile after the
    // profile's folder through the SDK's account_display_name.
#ifdef _WIN32
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
#else
    const std::string platform = "linux-x86_64";
    const std::string home     = "/home/jane";
#endif
    const auto discovered_name = [&](const std::string& qwen_home) {
        addon::Accounts_launcher_request request;
        request.launcher.script_body = "qwen";
        request.launcher.runner_key  = addon::k_runner_direct;
        request.launch_environment.platform_id = platform;
        request.launch_environment.bootstrap   = addon::Environment_bootstrap_evidence{qwen_home};
        request.default_home_fallback   = home;
        request.normalization_directory = home;
        Context discovery;
        discovery.m_request.operation = addon::k_accounts_discover_operation;
        discovery.m_request.payload   = addon::encode_accounts_launcher_request(request);
        qwen::handle_accounts(discovery);
        addon::Contract_failure failure;
        const auto accounts = discovery.m_result
            ? addon::decode_accounts_result(*discovery.m_result, &failure)
            : std::nullopt;
        return accounts && accounts->size() == 1 ? accounts->front().display_name : std::string{};
    };
    require(discovered_name(home + "/.qwen-work/") == ".qwen-work",
        "a trailing separator does not hide the profile folder's name");
    const std::string long_home = home + "/.qwen-account-for-the-varinomics-nightly-release-builds";
    require(discovered_name(long_home) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "  --resume <session>\n";
        return 0;
    }
    try { required_control_is_explicitly_unavailable(); launch_disposition(); resume_policy(argv[0]); account_names(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
