#include "qwen_preparation.h"
#include "qwen_profile.h"
#include "qwen_config.h"
#include "qwen_launcher.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { m_result = std::move(result); }
    void fail(addon::Result_status, std::string reason, std::string) override { throw std::runtime_error(reason); }
    bool finished() const override { return m_result.has_value(); }

    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
};

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
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

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "  --resume <session>\n";
        return 0;
    }
    try { launch_disposition(); resume_policy(argv[0]); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
