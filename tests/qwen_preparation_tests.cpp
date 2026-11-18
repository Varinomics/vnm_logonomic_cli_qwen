#include "qwen_preparation.h"
#include "qwen_accounts.h"
#include "qwen_profile.h"
#include "qwen_resume.h"
#include "qwen_config.h"
#include "qwen_launcher.h"
#include "qwen_json.h"
#include "qwen_peer.h"

#include <logonomic_cli_addon_test/invocation_context.h>

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_approval.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;

using Context = addon::test::Throwing_invocation_context;

void prepare(Context& context)
{
    context.m_request.deadline_ms = addon::limits::k_launch_prepare_deadline_ms;
    qwen::handle_preparation(context);
}

void require(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

void wake_patch(const addon::Launch_patch& patch)
{
    require(patch.session_wake.has_value(), "a negotiated managed launch reports wake readiness");
#ifdef __linux__
    const auto& wake = *patch.session_wake;
    const auto token = json_util::text(wake.binding, "launch_token");
    const auto entry = std::find(patch.set.begin(), patch.set.end(),
        std::pair<std::string, std::string>{qwen::k_wake_binding_environment, token});
    require(wake.available && token.size() == 32 && entry != patch.set.end(),
        "the exact live binding token reaches the original managed child environment");
#else
    require(!patch.session_wake->available && patch.session_wake->reason == "qwen_peer_platform_unavailable",
        "a Windows launch truthfully reports missing native peer inbox support");
#endif
}

void help_capabilities(const char* executable)
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("qwen-help-" + addon::random_nonce_hex());
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
#ifdef _WIN32
    const auto fake = root / "qwen.exe";
#else
    const auto fake = root / "qwen";
#endif
    fs::copy_file(fs::absolute(executable), fake);
    // Installed help mentions --continue/--resume before declaring --resume.
    // Admit the declaration while retaining option word boundaries.
    for (const auto& [help, supported] : std::vector<std::pair<const char*, bool>>{
            {"  --chat-recording  If false, --continue/--resume will not work.\n"
             "  -r, --resume  Resume a specific session by its ID.\n", true},
            {"  --chat-recording  If false, --continue/--resume will not work.\n", false},
            {"  --resumeless  A different option.\n", false},
            {"  prefix--resume  An embedded mention.\n", false},
        })
    {
        qwen::Environment environment{{"QWEN_TEST_HELP", help}};
        for (const auto* name : {"PATH", "SystemRoot", "SystemDrive", "PATHEXT", "ComSpec"}) {
            if (const auto* value = std::getenv(name)) environment.emplace_back(name, value);
        }
        Context context;
        require(qwen::selected_program_supports(addon::path_to_utf8(fake), environment,
            addon::path_to_utf8(root), {"--resume"}, context) == supported,
            "help option declarations remain usable after inline mentions, without admitting unsupported tokens");
    }
}

#ifdef _WIN32
std::string powershell_quote(std::string value)
{
    std::string result = "'";
    for (const char character : value) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + "'";
}

addon::Json_value run_resume_process(const addon::Resume_prepare_result& result,
    const addon::Resume_prepare_request& request, const std::filesystem::path& executable)
{
    auto command = std::vector<std::string>{addon::path_to_utf8(executable.parent_path() / "qwen_launch_bridge.exe")};
    command.insert(command.end(), result.process.argv.begin(), result.process.argv.end());
    Context context;
    const auto output = qwen::probe(command, result.patch.set, request.context.working_directory,
        addon::limits::k_resume_prepare_deadline_ms, context);
    require(output.has_value(), "the proposed bridge and PowerShell command must complete");
    std::ifstream input(addon::path_from_utf8(request.context.account_context->profile_path.value()) / "capture.json",
        std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(input), {}};
    const auto capture = addon::parse_json(content, {}, nullptr);
    require(capture.has_value(), "the fake Qwen child must record its executable, arguments and profile");
    return *capture;
}
#endif

void required_control_reaches_native_admission(const char* executable)
{
    // Regression: preparation refused every request that named required
    // actions, and the host always names them. The real preparation owner now
    // proposes native ACP with every Task server behind the host relay.
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("qwen-delegated-" + addon::random_nonce_hex());
    fs::create_directories(scratch / "selected");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{scratch};
    const auto root = fs::canonical(scratch);
    const auto home = addon::path_to_utf8(root / "selected");
#ifdef _WIN32
    const auto fake = root / "qwen.exe";
#else
    const auto fake = root / "qwen";
#endif
    fs::copy_file(fs::absolute(executable), fake);
    addon::Delegated_prepare_request request;
    request.purpose = addon::Structured_purpose::IMPLEMENTATION;
    request.context.launcher.runner_key  = addon::k_runner_direct;
    request.context.launcher.script_body = '"' + addon::path_to_utf8(fake) + '"';
    request.context.launcher_argv        = {addon::path_to_utf8(fake)};
    request.context.working_directory    = addon::path_to_utf8(root);
    request.context.account_context = addon::Account_context{2, qwen::account_key(root / "selected"), home};
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    for (const auto* name : {"SystemRoot", "PATH"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.context.launch_environment = environment;
    request.prompt = "Submit the review task to the delegation target.";
    request.control.task_id = "task-7f3a";
    request.control.private_artifact_root = root.generic_string();
    request.control.connection = addon::Delegated_connection{
        addon::path_to_utf8(root / "host relay"),
        addon::path_to_utf8(root / ".logonomic/task-servers.json")};
    const auto server_program = addon::path_to_utf8(root / "logonomic_coordination_mcp");
    request.control.servers.push_back({"coordination", server_program, {"status", "notice"},
        {{"VNM_CONTROL_TOKEN", "qwen-host-authority-canary"}}});
    request.control.servers.push_back({"delegation", server_program, {"targets", "submit"}, {}});
    Context context;
    context.m_request = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(request)};
    prepare(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_delegated_prepare_result(*context.m_result, &failure) : std::nullopt;
    require(result && result->acknowledgment.connection_version == addon::k_delegated_connection_version &&
        result->acknowledgment.server_ids == std::vector<std::string>{"coordination", "delegation"},
        "required actions reach a relay-acknowledged native proposal");
    require(!result->process.argv.empty() && result->process.argv.back() == "--acp" &&
        qwen::environment_value(result->process.environment, "QWEN_CODE_LEGACY_MCP_BLOCKING") == "1",
        "native ACP discovers the Task servers before it answers session/new");
    for (const auto& options : std::vector<std::vector<std::string>>{{"--yolo"}, {"-y"}, {"--yolo=true"},
            {"--yolo=false"}, {"--approval-mode", "default"},
            {"--approval-mode", "plan"}, {"--approval-mode", "auto-edit"}}) {
        auto selected = request;
        for (const auto& option : options) selected.context.launcher.script_body += " " + option;
        selected.context.launcher_argv.insert(selected.context.launcher_argv.end(), options.begin(), options.end());
        Context configured;
        configured.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        prepare(configured);
        const auto proposal = configured.m_result
            ? addon::decode_delegated_prepare_result(*configured.m_result, &failure) : std::nullopt;
        auto expected = selected.context.launcher_argv;
        expected.push_back("--acp");
        require(proposal && proposal->process.argv == expected, "launcher permission options are preserved in delegated ACP");
    }
    for (const bool override_model : {false, true}) {
        auto selected = request;
        const std::vector<std::string> options{"--model", "old-model", "--openai-api-key", "--model",
            "-m", "saved-model", "--approval-mode", "plan"};
        for (const auto& option : options) selected.context.launcher.script_body += " " + option;
        selected.context.launcher_argv.insert(selected.context.launcher_argv.end(), options.begin(), options.end());
        if (override_model) selected.model_selection = addon::Delegated_model_selection{"task-model", "high"};
        Context configured;
        configured.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        prepare(configured);
        const auto proposal = configured.m_result
            ? addon::decode_delegated_prepare_result(*configured.m_result, &failure) : std::nullopt;
        auto expected = override_model
            ? std::vector<std::string>{addon::path_to_utf8(fake), "--openai-api-key", "--model", "--approval-mode", "plan",
                "--model", "task-model", "--fallback-model", "task-model"}
            : selected.context.launcher_argv;
        expected.push_back("--acp");
        require(proposal && proposal->process.argv == expected && proposal->model_selection == selected.model_selection &&
            proposal->account_context && proposal->account_context->profile_path == home &&
            proposal->account_context->adapter_account_key == selected.context.account_context->adapter_account_key &&
            qwen::environment_value(proposal->process.environment, "QWEN_HOME") == home,
            "requested model replaces saved aliases only and preserves account, permission mode and opaque option values");
        require(selected.context.launcher_argv[selected.context.launcher_argv.size() - 3] == "saved-model",
            "task settings do not mutate the saved launcher");
    }
    for (const std::string model : {"-other-option", " first-model", "first-model,second-model"}) {
        auto selected = request;
        selected.model_selection = addon::Delegated_model_selection{model, std::nullopt};
        Context refused;
        refused.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        bool scoped_refusal = false;
        try { prepare(refused); }
        catch (const std::runtime_error& error) { scoped_refusal = std::string(error.what()) == "model_selection_unavailable"; }
        require(scoped_refusal && !refused.m_result,
            "model syntax that cannot preserve sole-model fallback is refused before launch");
    }
    {
        auto selected = request;
        selected.model_selection = addon::Delegated_model_selection{std::nullopt, "high"};
        Context configured;
        configured.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        prepare(configured);
        const auto proposal = configured.m_result
            ? addon::decode_delegated_prepare_result(*configured.m_result, &failure) : std::nullopt;
        require(proposal && proposal->process.argv == result->process.argv && proposal->process.environment == result->process.environment,
            "effort-only selection leaves launcher, fallback and retry defaults unchanged");
        selected.model_selection->model = "task-model";
        selected.context.launch_environment->effective_environment.set("QWEN_TEST_HELP", addon::Json_value("  --acp\n  --model <name>\n"));
        Context unavailable;
        unavailable.m_request = {"delegated", {"delegated-session", 3}, "prepare",
            addon::encode_delegated_prepare_request(selected)};
        bool scoped_refusal = false;
        try { prepare(unavailable); }
        catch (const std::runtime_error& error) { scoped_refusal = std::string(error.what()) == "model_selection_unavailable"; }
        require(scoped_refusal && !unavailable.m_result,
            "an installed CLI without sole-model fallback support cannot advertise an applied model");
    }
    const auto& input = result->process.stdin_payload;
    require(input.find("qwen-host-authority-canary") == std::string::npos && input.find(request.prompt) == std::string::npos,
        "neither host credentials nor the task enter ACP stdin before admission");
    const auto session = addon::parse_json(input.substr(input.find('\n') + 1), {}, nullptr);
    require(session.has_value(), "native session request remains JSON");
    const auto& servers = session->find("params")->find("mcpServers")->as_array();
    require(servers.size() == 2 && result->private_artifacts.size() == 2, "every Task server is configured once");
    for (std::size_t index = 0; index < servers.size(); ++index) {
        const auto& server = servers[index];
        const auto& args = server.find("args")->as_array();
        const auto& artifact = result->private_artifacts[index];
        require(server.find("name")->as_string() == "logonomic-task-task-7f3a-" + request.control.servers[index].server_id &&
            server.find("command")->as_string() == request.control.connection->relay_program &&
            server.find("env")->as_array().empty() && args.size() == 8 &&
            args[0].as_string() == "--descriptor" && args[1].as_string() == request.control.connection->descriptor_path &&
            args[2].as_string() == "--server" && args[3].as_string() == request.control.servers[index].server_id &&
            args[4].as_string() == "--attestation-path" && args[6].as_string() == "--attestation-nonce",
            "ACP uses exact nonsecret relay arguments with a catalog receipt");
        const auto& receipt = args[5].as_string();
        const auto& nonce = args[7].as_string();
        require(receipt == request.control.private_artifact_root + "/mcp/server-" + nonce + ".ready" &&
            artifact.relative_path == "mcp/server-" + nonce + ".connection" && artifact.content == nonce,
            "each receipt sits in a materialized private folder in the host's '/' form");
    }

    request.control.connection.reset();
    Context unconnected;
    unconnected.m_request = {"delegated", {"delegated-session", 3}, "prepare", addon::encode_delegated_prepare_request(request)};
    bool refused = false;
    try { prepare(unconnected); }
    catch (const std::runtime_error& error) { refused = std::string(error.what()) == "task_server_connection_unavailable"; }
    require(refused && unconnected.m_failure_status == addon::Result_status::UNAVAILABLE && !unconnected.m_result,
        "Task servers are never launched without the host relay");
}

void launch_disposition(const char* executable)
{
    // Oracle: the launch-preparation@3 decoder requires explicit feature
    // admission. Exercise the actual provider producer without running a CLI.
    addon::Prepare_request request;
    request.working_directory = addon::path_to_utf8(std::filesystem::current_path());
#ifdef _WIN32
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("qwen-launch-" + addon::random_nonce_hex());
    fs::create_directories(root / "selected");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    fs::copy_file(fs::absolute(executable), root / "qwen.exe");
    request.launcher = {"qwen --model 'selected-model' --approval-mode 'default'", addon::k_runner_powershell, ""};
    request.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", request.launcher.script_body};
    request.working_directory = addon::path_to_utf8(root);
#else
    (void)executable;
    request.launcher = {"qwen", addon::k_runner_bash, ""};
    request.launcher_argv = {"bash", "-c", "qwen"};
#endif
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
#ifdef _WIN32
    const auto home = addon::path_to_utf8(root / "selected");
    environment.effective_environment.set("PATH", addon::Json_value(
        addon::path_to_utf8(root) + ";" + (std::getenv("PATH") ? std::getenv("PATH") : "")));
    for (const auto* name : {"SystemRoot", "SystemDrive", "PATHEXT", "ComSpec", "PSModulePath"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
#else
    const auto home = addon::path_to_utf8(std::filesystem::current_path() / ".qwen-profile");
#endif
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.launch_environment = environment;
    request.account_context = addon::Account_context{2, qwen::account_key(addon::path_from_utf8(home)), home};
    request.session_wake = addon::Session_wake_offer{};
    for (const bool prepared : {true, false}) {
        if (!prepared) {
            request.launcher.script_body = "echo unrelated";
#ifdef _WIN32
            request.launcher_argv.back() = request.launcher.script_body;
#else
            request.launcher_argv = {"bash", "-c", "echo unrelated"};
#endif
        }
        Context context;
        context.m_request.capability = {"launch-preparation", 3};
        context.m_request.operation = "prepare";
        context.m_request.payload = addon::encode_prepare_request(request, 3);
        prepare(context);
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
        if (prepared) {
            wake_patch(*patch);
            require(qwen::environment_value(patch->set, qwen::k_profile_home_environment) == home,
                "native hook provenance receives the exact bootstrap-selected profile");
        }
        if (!prepared) require(patch->set.empty() && patch->unset.empty() && !patch->argv,
            "unmanaged preparation must preserve the native launch");
    }
#ifdef _WIN32
    for (const auto& script : {"'qwen.exe' --model selected-model", "qwen --model $env:SELECTED_MODEL"}) {
        request.launcher.script_body = script;
        request.launcher_argv.back() = script;
        Context context;
        context.m_request.capability = {"launch-preparation", 3};
        context.m_request.operation = "prepare";
        context.m_request.payload = addon::encode_prepare_request(request, 3);
        prepare(context);
        addon::Contract_failure failure;
        const auto patch = context.m_result ? addon::decode_launch_patch(*context.m_result, &failure, 3) : std::nullopt;
        require(patch && patch->disposition && !patch->disposition->prepared,
            "invalid or dynamic PowerShell launchers must not be offered structured preparation");
    }
#endif
}

#ifdef _WIN32
void direct_launch_starts_selected_program(const char* executable)
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("qwen-direct-" + addon::random_nonce_hex());
    fs::create_directories(scratch / "bin");
    fs::create_directories(scratch / "native");
    fs::create_directories(scratch / "selected");
    const auto root = fs::canonical(scratch);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    const auto fake = root / "native" / "qwen.exe";
    const auto home = addon::path_to_utf8(root / "selected");
    fs::copy_file(fs::absolute(executable), fake);
    {
        std::ofstream command(root / "bin" / "qwen.cmd", std::ios::binary);
        command << "@echo off\r\n\"" << addon::path_to_utf8(fake) << "\" %*\r\n";
        std::ofstream script(root / "bin" / "qwen.ps1", std::ios::binary);
        script << "& " << powershell_quote(addon::path_to_utf8(fake)) << " @args\nexit $LASTEXITCODE\n";
    }
    addon::Launch_environment environment;
    environment.platform_id = "windows-x86_64";
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.effective_environment.set("PATH", addon::Json_value(
        addon::path_to_utf8(root / "bin") + ";" + (std::getenv("PATH") ? std::getenv("PATH") : "")));
    for (const auto* name : {"SystemRoot", "SystemDrive", "PATHEXT", "ComSpec", "PSModulePath"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    const std::vector<std::string> options{"--model", "selected-model", "--approval-mode", "plan"};
    for (const auto& program : std::vector<std::string>{"qwen", addon::path_to_utf8(root / "bin" / "qwen.cmd"),
            addon::path_to_utf8(root / "bin" / "qwen.ps1"), addon::path_to_utf8(fake)})
    {
        addon::Prepare_request request;
        request.launcher = {'"' + program + "\" --model selected-model --approval-mode plan", addon::k_runner_direct, ""};
        request.launcher_argv = {program};
        request.launcher_argv.insert(request.launcher_argv.end(), options.begin(), options.end());
        request.working_directory = addon::path_to_utf8(root);
        request.account_context = addon::Account_context{2, qwen::account_key(root / "selected"), home};
        request.launch_environment = environment;
        Context context;
        context.m_request = {"direct", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3)};
        prepare(context);
        addon::Contract_failure failure;
        const auto patch = context.m_result ? addon::decode_launch_patch(*context.m_result, &failure, 3) : std::nullopt;
        require(patch && patch->argv && patch->disposition && patch->disposition->prepared,
            "the selected direct launcher must propose a Windows native invocation");
        require(addon::validate_launch_patch(*patch, {}, {}, &failure), "the native invocation satisfies host policy");
        require(patch->account_context && patch->account_context->adapter_account_key == request.account_context->adapter_account_key &&
            patch->account_context->profile_path == request.account_context->profile_path,
            "the direct launch retains its selected account and profile");
        std::error_code ignored;
        fs::remove(root / "selected" / "capture.json", ignored);
        const auto output = qwen::probe(*patch->argv, qwen::environment_pairs(environment.effective_environment),
            request.working_directory, addon::limits::k_launch_prepare_deadline_ms, context);
        require(output.has_value(), "the actual fresh-launch invocation completes through CreateProcessW");
        std::ifstream input(root / "selected" / "capture.json", std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(input), {}};
        const auto capture = addon::parse_json(content, {}, nullptr);
        require(capture && addon::path_from_utf8(json_util::text(*capture, "executable")) == fs::absolute(fake) &&
            json_util::text(*capture, "profile") == home, "the selected child receives the original native profile");
        std::vector<std::string> actual;
        for (const auto& argument : capture->find("argv")->as_array()) {
            actual.push_back(argument.as_string());
        }
        require(actual == options, "fresh script-shim launch preserves model and permission arguments exactly");
        std::cout << "fresh direct child=" << json_util::text(*capture, "executable")
            << " argv=" << addon::serialize_json(*capture->find("argv")) << '\n';
    }
}
#endif

void resume_policy(const char* executable)
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("qwen-resume-" + addon::random_nonce_hex());
    fs::create_directories(scratch / "selected");
    const auto root = fs::canonical(scratch);
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
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    environment.effective_environment.set("OPENAI_API_KEY", addon::Json_value("frozen-fixture-key"));
    for (const auto* name : {"SystemRoot", "SystemDrive", "PATH", "PATHEXT", "ComSpec", "PSModulePath"}) {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    request.context.launch_environment = environment;
    request.resume_record.set("provider_session_id", addon::Json_value("019933a1-6e43-7102-a520-b3c8294cc763"));
    require(!fs::exists(root / "selected" / "projects"),
        "legacy resume fixture has no native transcript evidence");
    Context context;
    context.m_request = {"resume", {"interactive-resume", 2}, "prepare_resume", addon::encode_resume_prepare_request(request, 2)};
    prepare(context);
    addon::Contract_failure failure;
    const auto result = context.m_result ? addon::decode_resume_prepare_result(*context.m_result, &failure, 2) : std::nullopt;
    require(result && addon::validate_resume_process_proposal(result->process, &failure) &&
        addon::validate_launch_patch(result->patch, {}, {}, &failure), "resume producer must satisfy terminal policy");
    auto expected_environment = qwen::environment_pairs(environment.effective_environment);
    qwen::set_environment(expected_environment, std::string(qwen::k_profile_home_environment), home);
    require(result->patch.set == expected_environment,
        "resume retains the frozen authentication environment and supplies its resolved hook profile");
    require(result->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
        addon::validate_resume_acknowledgment(request.resume_record, result->acknowledgment, &failure),
        "resume must preserve the selected native session identity");
    request.context.session_wake = addon::Session_wake_offer{};
    Context wake_context;
    wake_context.m_request = {"resume-wake", {"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(request, 2)};
    prepare(wake_context);
    const auto wake_result = wake_context.m_result
        ? addon::decode_resume_prepare_result(*wake_context.m_result, &failure, 2) : std::nullopt;
    require(wake_result.has_value(), "resume preserves its independently negotiated wake result");
    wake_patch(wake_result->patch);
    request.context.session_wake.reset();
    {
        auto retained = request;
        const auto runtime = root / "native-runtime";
        retained.context.launch_environment->effective_environment.set("QWEN_RUNTIME_DIR",
            addon::Json_value(addon::path_to_utf8(runtime)));
        // Native sanitizeCwd replaces each non-alphanumeric UTF-16 unit;
        // its Windows input is lowercased before that replacement.
        std::string project;
        for (auto unit : root.u16string()) {
#ifdef _WIN32
            if (unit >= u'A' && unit <= u'Z') {
                unit += u'a' - u'A';
            }
            if (unit == u'\u212a') {
                unit = u'k';
            }
            else
            if (unit == u'\u0130') {
                project += "i-";
                continue;
            }
#endif
            project += (unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z') ||
                (unit >= u'0' && unit <= u'9') ? static_cast<char>(unit) : '-';
        }
        const auto transcript = runtime / "projects" / project / "chats" /
            "019933a1-6e43-7102-a520-b3c8294cc763.jsonl";
        fs::create_directories(transcript.parent_path());
        auto turn = *addon::parse_json(R"({"type":"user","uuid":"first-turn","parentUuid":null,
            "message":{"role":"user","parts":[{"text":"Retained conversation"}]}})", {}, nullptr);
        turn.set("sessionId", addon::Json_value("019933a1-6e43-7102-a520-b3c8294cc763"));
        turn.set("cwd", addon::Json_value(request.context.working_directory));
        std::ofstream transcript_output(transcript, std::ios::binary);
        transcript_output << addon::serialize_json(turn) << '\n';
        transcript_output.close();
        require(bool(transcript_output), "native resume fixture is complete");
        const auto native = json_util::object({
            {"transcript_path", addon::Json_value(addon::path_to_utf8(transcript))},
            {"cwd", addon::Json_value(request.context.working_directory)},
            {"profile_home", addon::Json_value(home)}});
        retained.resume_record = qwen::make_resume_record("019933a1-6e43-7102-a520-b3c8294cc763", native);
        require(retained.resume_record.find("native_source"), "retained resume fixture carries native scope");
        retained.context.working_directory = addon::path_to_utf8(root / "selected" / ".." / ".") + "//";
#ifdef _WIN32
        std::replace(retained.context.working_directory.begin(), retained.context.working_directory.end(), '\\', '/');
#endif
        Context retained_context;
        retained_context.m_request = {"resume-retained", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(retained, 2)};
        prepare(retained_context);
        const auto proposal = retained_context.m_result
            ? addon::decode_resume_prepare_result(*retained_context.m_result, &failure, 2) : std::nullopt;
        require(proposal && proposal->process.argv.back() == "019933a1-6e43-7102-a520-b3c8294cc763" &&
            addon::validate_resume_acknowledgment(retained.resume_record, proposal->acknowledgment, &failure) &&
            qwen::environment_value(proposal->patch.set, "QWEN_RUNTIME_DIR") == addon::path_to_utf8(runtime) &&
            qwen::environment_value(proposal->patch.set, qwen::k_profile_home_environment) == home,
            "equivalent directory spelling preserves the exact native runtime and selected record");
    }
    {
        auto mismatched = request;
        const auto native = json_util::object({
            {"transcript_path", addon::Json_value(addon::path_to_utf8(root / "other-runtime" / "projects" /
                "project" / "chats" / "019933a1-6e43-7102-a520-b3c8294cc763.jsonl"))},
            {"cwd", addon::Json_value(request.context.working_directory)},
            {"profile_home", addon::Json_value(addon::path_to_utf8(root / "other-profile"))}});
        mismatched.resume_record = qwen::make_resume_record("019933a1-6e43-7102-a520-b3c8294cc763", native);
        Context mismatched_context;
        mismatched_context.m_request = {"resume-wrong-profile", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(mismatched, 2)};
        bool rejected = false;
        try {
            prepare(mismatched_context);
        }
        catch (const std::exception& error) {
            rejected = std::string_view(error.what()) == "qwen_resume_scope_mismatch";
        }
        require(rejected && !mismatched_context.m_result,
            "explicit resume refuses a retained native identity from another prepared profile");
    }
#ifdef _WIN32
    const auto direct_capture = run_resume_process(*result, request, fs::absolute(executable));
    const auto* direct_arguments = direct_capture.find("argv");
    require(direct_arguments && direct_arguments->is_array() &&
        addon::path_from_utf8(json_util::text(direct_capture, "executable")) == fs::absolute(fake) &&
        json_util::text(direct_capture, "profile") == home &&
        direct_arguments->as_array() == addon::Json_value::Array{
            addon::Json_value("--resume"), addon::Json_value("019933a1-6e43-7102-a520-b3c8294cc763")},
        "direct resume must execute the selected Qwen file with its profile and exact ID");
    std::cout << "direct resume child=" << json_util::text(direct_capture, "executable")
        << " profile=" << json_util::text(direct_capture, "profile")
        << " argv=" << addon::serialize_json(*direct_arguments) << '\n';
    const auto shims = root / "shims";
    fs::create_directories(shims);
    {
        std::ofstream script(shims / "qwen.ps1");
        script << "Set-Content -LiteralPath (Join-Path $env:QWEN_HOME 'resolved.txt') -Value 'ps1'\n"
            << "& " << powershell_quote(addon::path_to_utf8(fake)) << " @args\n"
            << "exit $LASTEXITCODE\n";
        std::ofstream batch(shims / "qwen.cmd");
        batch << "@echo off\r\n"
            << "echo cmd> \"%QWEN_HOME%\\resolved.txt\"\r\n"
            << "\"" << addon::path_to_utf8(fake) << "\" %*\r\n";
    }
    environment.effective_environment.set("PATH", addon::Json_value(
        addon::path_to_utf8(shims) + ";" + (std::getenv("PATH") ? std::getenv("PATH") : "")));
    request.context.launch_environment = environment;
    const std::string session_id = "019933a1-6e43-7102-a520-b3c8294cc763";
    const std::string upstream_script = "& " + powershell_quote(addon::path_to_utf8(shims / "qwen.ps1")) +
        " --model 'selected-model'";
    {
        auto persistent = request;
        persistent.context.launcher = {upstream_script, addon::k_runner_powershell, ""};
        persistent.context.launcher_argv = {"powershell.exe", "-NoExit", "-NoLogo", "-Command", upstream_script};
        Context persistent_context;
        persistent_context.m_request = {"persistent-resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(persistent, 2)};
        prepare(persistent_context);
        const auto proposal = persistent_context.m_result
            ? addon::decode_resume_prepare_result(*persistent_context.m_result, &failure, 2) : std::nullopt;
        require(proposal &&
            std::find(proposal->process.argv.begin(), proposal->process.argv.end(), "-NoExit") != proposal->process.argv.end() &&
            !proposal->process.argv.back().ends_with("; exit $LASTEXITCODE"),
            "resuming preserves an explicitly persistent PowerShell runner");
    }
    {
        addon::Simulator_options options;
        options.package_directory = fs::absolute(executable).parent_path() / "package" / "logonomic.cli.qwen";
        options.scratch_directory = root / "adapter-scratch";
        options.profile_storage_root = root / "adapter-profiles";
        addon::Host_simulator host(std::move(options), nullptr);
        addon::Contract_failure startup_failure;
        require(host.start(&startup_failure), "packaged Qwen adapter must start for upstream admission");
        const auto invoke = [&](const addon::Capability_pair& capability, const char* operation,
            addon::Json_value payload) {
            const auto id = host.invoke(capability, operation, std::move(payload), 15000);
            const auto outcome = host.wait_outcome(id, 18000);
            require(outcome.has_value() && outcome->status == addon::Host_status::SUCCESS,
                "packaged upstream invocation must settle successfully");
            return outcome->payload;
        };
        addon::Launcher_request selected_launcher{upstream_script, addon::k_runner_powershell, ""};
        const auto& sidecars = host.package()->sidecars;
        const auto sidecar = std::find_if(sidecars.begin(), sidecars.end(), [](const auto& candidate) {
            return candidate.capability_id == "launcher-discovery" && candidate.version == 1;
        });
        require(sidecar != sidecars.end(), "packaged launcher discovery must be declared");
        const auto declaration = addon::decode_launcher_discovery_declaration(sidecar->payload, &failure);
        require(declaration.has_value(), "packaged launcher discovery environment must be readable");
        const auto ambient = addon::ambient_view(qwen::environment_pairs(environment.effective_environment),
            *declaration, environment.platform_id);
        const auto recognition_payload = invoke({"launcher-discovery", 1}, "recognize",
            addon::encode_recognize_request(selected_launcher, ambient, environment.platform_id));
        const auto recognition = addon::decode_recognize_result(recognition_payload, &failure);
        addon::Accounts_launcher_request account_request;
        account_request.launcher = selected_launcher;
        account_request.launch_environment = environment;
        account_request.default_home_fallback = home;
        account_request.normalization_directory = addon::path_to_utf8(root);
        const auto discovery_payload = invoke({"accounts", 2}, "discover",
            addon::encode_accounts_launcher_request(account_request));
        const auto accounts = addon::decode_accounts_result(discovery_payload, &failure);
        const auto resolution_payload = invoke({"accounts", 2}, "resolve",
            addon::encode_accounts_launcher_request(account_request));
        const auto resolution = addon::decode_account_resolution_result(resolution_payload, &failure);
        std::cout << "call-operator upstream provider=" << (recognition && recognition->recognized)
            << " discovered=" << (accounts ? accounts->size() : 0)
            << " resolved=" << (resolution && resolution->adapter_account_key
                ? *resolution->adapter_account_key : "absent") << '\n';
        require(recognition && recognition->recognized && recognition->display_name == "Qwen Code" &&
            accounts && accounts->size() == 1 && accounts->front().adapter_account_key ==
                request.context.account_context->adapter_account_key &&
            resolution && resolution->adapter_account_key == request.context.account_context->adapter_account_key,
            "supported PowerShell launcher must retain provider and account identity upstream");
        selected_launcher.script_body = "qwen";
        account_request.launcher = selected_launcher;
        const auto bare_recognition = addon::decode_recognize_result(
            invoke({"launcher-discovery", 1}, "recognize",
                addon::encode_recognize_request(selected_launcher, ambient, environment.platform_id)), &failure);
        const auto bare_resolution = addon::decode_account_resolution_result(
            invoke({"accounts", 2}, "resolve", addon::encode_accounts_launcher_request(account_request)), &failure);
        require(bare_recognition && bare_recognition->recognized && bare_resolution &&
            bare_resolution->adapter_account_key == request.context.account_context->adapter_account_key,
            "bare PowerShell Qwen must retain provider and account identity upstream");
        std::cout << "bare upstream provider=1 resolved=" << *bare_resolution->adapter_account_key << '\n';
        const auto enabled = addon::decode_launcher_approval_transform_result(
            invoke({addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
                addon::k_launcher_approval_transform_operation,
                addon::encode_launcher_approval_transform_request({selected_launcher, environment.platform_id,
                    addon::Launcher_approval_action::YOLOFY})), &failure);
        require(enabled && enabled->script_body == "qwen --yolo=true",
            "packaged Yolofy emits the native attached boolean spelling");
        for (const auto* approval : {"--yolo=true", "--yolo=false"}) {
            selected_launcher.script_body = std::string_view(approval) == "--yolo=true"
                ? enabled->script_body : std::string("qwen ") + approval;
            const auto recognized = addon::decode_recognize_result(
                invoke({"launcher-discovery", 1}, "recognize",
                    addon::encode_recognize_request(selected_launcher, ambient, environment.platform_id)), &failure);
            addon::Prepare_request launch;
            launch.launcher = selected_launcher;
            launch.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", selected_launcher.script_body};
            launch.working_directory = addon::path_to_utf8(root);
            launch.launch_environment = environment;
            launch.account_context = request.context.account_context;
            const auto prepared = addon::decode_launch_patch(
                invoke({"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch, 3)), &failure, 3);
            require(recognized && recognized->recognized && prepared && prepared->disposition &&
                prepared->disposition->prepared,
                "generated explicit approval spellings retain packaged PowerShell recognition and preparation");
        }
        for (const auto& script : {
                powershell_quote(addon::path_to_utf8(shims / "qwen.ps1")) + " --model selected-model",
                std::string("qwen --model $env:SELECTED_MODEL"),
                std::string("qwen --model selected-model | Out-Null"),
                std::string("qwen --unsupported-option")})
        {
            selected_launcher.script_body = script;
            account_request.launcher = selected_launcher;
            const auto rejected_recognition = addon::decode_recognize_result(
                invoke({"launcher-discovery", 1}, "recognize",
                    addon::encode_recognize_request(selected_launcher, ambient, environment.platform_id)), &failure);
            const auto rejected_resolution = addon::decode_account_resolution_result(
                invoke({"accounts", 2}, "resolve", addon::encode_accounts_launcher_request(account_request)), &failure);
            require(rejected_recognition && !rejected_recognition->recognized && rejected_resolution &&
                !rejected_resolution->adapter_account_key,
                "invalid or dynamic PowerShell scripts must be refused before preparation");
            std::cout << "upstream rejected script=" << script << '\n';
        }
    }
    int failures = 0;
    const auto run_case = [&](const char* name, const std::string& script,
        const std::vector<std::string>& expected, bool shim) {
        try {
            std::error_code ignored;
            fs::remove(root / "selected" / "capture.json", ignored);
            fs::remove(root / "selected" / "resolved.txt", ignored);
            request.context.launcher = {script, addon::k_runner_powershell, ""};
            request.context.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", script};
            Context shell_context;
            shell_context.m_request = {"resume", {"interactive-resume", 2}, "prepare_resume",
                addon::encode_resume_prepare_request(request, 2)};
            prepare(shell_context);
            const auto prepared = addon::decode_resume_prepare_result(*shell_context.m_result, &failure, 2);
            require(prepared && addon::validate_resume_process_proposal(prepared->process, &failure) &&
                addon::validate_resume_acknowledgment(request.resume_record, prepared->acknowledgment, &failure),
                "resume proposal and exact record acknowledgment must satisfy the SDK");
            const auto capture = run_resume_process(*prepared, request, fs::absolute(executable));
            const auto* arguments = capture.find("argv");
            require(arguments && arguments->is_array() &&
                addon::path_from_utf8(json_util::text(capture, "executable")) == fs::absolute(fake) &&
                json_util::text(capture, "profile") == home,
                "the fake child must retain its executable and Qwen profile");
            std::vector<std::string> actual;
            for (const auto& item : arguments->as_array()) {
                require(item.is_string(), "captured child arguments must be strings");
                actual.push_back(item.as_string());
            }
            require(actual == expected, "PowerShell must deliver the selected arguments and exact resume ID");
            if (shim) {
                std::ifstream marker(root / "selected" / "resolved.txt");
                std::string selected;
                marker >> selected;
                require(selected == "ps1", "bare Qwen must use PowerShell's selected script, not the cmd shim");
            }
            std::cout << name << " child=" << json_util::text(capture, "executable")
                << " profile=" << json_util::text(capture, "profile")
                << " argv=" << addon::serialize_json(*arguments)
                << " selection=" << (shim ? "powershell-qwen.ps1" : "call-operator-qwen.exe") << '\n';
        }
        catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    const auto reject_case = [&](const char* name, const std::string& script) {
        request.context.launcher = {script, addon::k_runner_powershell, ""};
        request.context.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", script};
        Context shell_context;
        shell_context.m_request = {"resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(request, 2)};
        try {
            prepare(shell_context);
            std::cerr << name << ": invalid or dynamic PowerShell script was accepted\n";
            ++failures;
        }
        catch (const std::runtime_error& error) {
            if (std::string(error.what()) != "resume_selector_unavailable") {
                std::cerr << name << ": " << error.what() << '\n';
                ++failures;
            }
            else {
                std::cout << name << " rejected=resume_selector_unavailable\n";
            }
        }
    };
    run_case("bare qwen", "qwen", {"--resume", session_id}, true);
    run_case("bare single quotes", "qwen --model 'selected-model' --approval-mode 'default'"
        " --openai-base-url 'http://127.0.0.1:18111/v1' --openai-api-key 'fixture-auth'",
        {"--model", "selected-model", "--approval-mode", "default",
            "--openai-base-url", "http://127.0.0.1:18111/v1",
            "--openai-api-key", "fixture-auth", "--resume", session_id}, true);
    run_case("parenthesized approval", "qwen --approval-mode ('default')",
        {"--approval-mode", "default", "--resume", session_id}, true);
    run_case("explicit bypass", "qwen --yolo=true",
        {"--yolo=true", "--resume", session_id}, true);
    run_case("explicit inactive bypass", "qwen --yolo=false",
        {"--yolo=false", "--resume", session_id}, true);
    run_case("call operator", "& " + powershell_quote(addon::path_to_utf8(fake)) + " --model 'selected-model'",
        {"--model", "selected-model", "--resume", session_id}, false);
    run_case("call operator script", upstream_script,
        {"--model", "selected-model", "--resume", session_id}, true);
    reject_case("quoted head without call operator", powershell_quote(addon::path_to_utf8(fake)) +
        " --model 'selected-model'");
    reject_case("dynamic value", "qwen --model $env:SELECTED_MODEL");
    reject_case("unrelated pipeline", "qwen --model selected | Out-Null");
    require(failures == 0, "PowerShell resume boundary cases must match the installed shell");
#endif
}

} // namespace

void account_names()
{
    // Oracle: accounts@2 names an account with a filesystem profile after the
    // profile's folder through the SDK's account_display_name.
#ifdef _WIN32
    const std::string platform = "windows-x86_64";
    const std::string home     = "C:/Users/Jane Doe";
#else
    const std::string platform = addon::host_platform_id();
    const std::string home     = "/home/jane";
#endif
    const auto discovered_account = [&](const std::string& qwen_home) -> std::optional<addon::Account_record> {
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
        return accounts && accounts->size() == 1 ? std::optional(accounts->front()) : std::nullopt;
    };
    const auto discovered_name = [&](const std::string& qwen_home) {
        const auto account = discovered_account(qwen_home);
        return account ? account->display_name : std::string{};
    };
    require(discovered_name(home + "/.qwen-work/") == ".qwen-work",
        "a trailing separator does not hide the profile folder's name");
    const std::string long_home = home + "/.qwen-account-for-the-varinomics-nightly-release-builds";
    require(discovered_name(long_home) == addon::account_display_name(long_home),
        "an over-long folder name is fitted by the SDK's account display-name rule");
#ifdef _WIN32
    // A profile on a share that cannot be reached is an unavailable account;
    // it must not fail the discovery of every account.
    const auto unreachable = discovered_account("\\\\localhost\\logonomic-absent-share\\");
    require(unreachable && unreachable->display_name == "\\\\localhost\\logonomic-absent-share" &&
        unreachable->status == addon::Account_status::UNAVAILABLE,
        "a profile on an unreachable share is discovered as an unavailable account");
#endif
}

int main(int argc, char** argv)
{
    if (qwen::base_name(argv[0]) == "qwen") {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            const char* help = std::getenv("QWEN_TEST_HELP");
            std::cout << (help ? help : "  --resume <session>\n  --acp\n  --model <name>\n  --fallback-model <name>\n");
            return 0;
        }
        const char* home = std::getenv("QWEN_HOME");
        if (!home) return 2;
        addon::Json_value capture = addon::Json_value::object();
        capture.set("executable", addon::Json_value(addon::path_to_utf8(std::filesystem::absolute(argv[0]))));
        capture.set("profile", addon::Json_value(home));
        addon::Json_value arguments = addon::Json_value::array();
        for (int index = 1; index < argc; ++index) arguments.as_array().push_back(addon::Json_value(argv[index]));
        capture.set("argv", std::move(arguments));
        std::ofstream output(addon::path_from_utf8(home) / "capture.json", std::ios::binary);
        output << addon::serialize_json(capture);
        return output ? 0 : 3;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "  --resume <session>\n  --acp\n";
        return 0;
    }
    try {
        help_capabilities(argv[0]);
        required_control_reaches_native_admission(argv[0]);
        launch_disposition(argv[0]);
#ifdef _WIN32
        direct_launch_starts_selected_program(argv[0]);
#endif
        resume_policy(argv[0]);
        account_names();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
