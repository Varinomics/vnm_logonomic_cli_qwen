#include "qwen_preparation.h"
#include "qwen_json.h"
#include "qwen_launcher.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon_test/invocation_context.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Context = addon::test::Throwing_invocation_context;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Scratch
{
    fs::path path = fs::temp_directory_path() / ("qwen-initial-" + addon::random_nonce_hex());
    ~Scratch()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

#ifdef _WIN32
std::string quote(std::string_view value)
{
    std::string result = "'";
    for (const char character : value) {
        result += character;
        if (character == '\'') {
            result += '\'';
        }
    }
    return result + '\'';
}
#endif

std::optional<addon::Launch_patch> prepare(const addon::Prepare_request& request, Context& context)
{
    context.m_request = {"initial", {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request)};
    qwen::handle_preparation(context);
    addon::Contract_failure failure;
    return context.m_result ? addon::decode_launch_patch(*context.m_result, &failure) : std::nullopt;
}

void require_refusal(addon::Prepare_request request)
{
    Context context;
    bool refused = false;
    try {
        (void)prepare(request, context);
    }
    catch (const std::runtime_error& error) {
        refused = std::string(error.what()) == addon::reason::k_initial_prompt_unavailable;
    }
    require(refused && context.m_failure_status == addon::Result_status::UNAVAILABLE && !context.m_result,
        "an unavailable initial prompt must fail rather than start an ordinary session");
}

void initial_prompt_delivery(const fs::path& executable)
{
    // Oracle: interactive-initial-prompt@1 and Qwen's --prompt-interactive
    // contract. A native recording child exercises the actual proposed argv.
    Scratch scratch;
    fs::create_directories(scratch.path / "selected");
    const auto root = fs::canonical(scratch.path);
#ifdef _WIN32
    const auto fake = root / "qwen.exe";
#else
    const auto fake = root / "qwen";
#endif
    fs::copy_file(executable, fake);
    const auto home = addon::path_to_utf8(root / "selected");
    addon::Launch_environment environment;
    environment.platform_id = addon::host_platform_id();
    environment.bootstrap = addon::Environment_bootstrap_evidence{home};
    environment.effective_environment.set("QWEN_HOME", addon::Json_value(home));
    for (const auto* name : {"PATH", "SystemRoot", "SystemDrive", "PATHEXT", "ComSpec",
            "PSModulePath", "TEMP", "TMP"})
    {
        if (const auto* value = std::getenv(name)) {
            environment.effective_environment.set(name, addon::Json_value(value));
        }
    }
    addon::Prepare_request direct;
    direct.launcher = {'"' + addon::path_to_utf8(fake) + "\" --model chosen-model --approval-mode plan",
        addon::k_runner_direct, ""};
    const std::vector<std::string> options{"--model", "chosen-model", "--approval-mode", "plan"};
    direct.launcher_argv = {addon::path_to_utf8(fake)};
    direct.launcher_argv.insert(direct.launcher_argv.end(), options.begin(), options.end());
    direct.working_directory = addon::path_to_utf8(root);
    direct.launch_environment = environment;
    direct.account_context = addon::Account_context{2, qwen::account_key(root / "selected"), home};
    const auto brief = root / u8"continuation briefs" / u8"Zoë 概要" / "brief.md";
    fs::create_directories(brief.parent_path());
    std::ofstream(brief, std::ios::binary) << "A retained continuation brief.\n";
    const std::string text = "Logonomic continuation: read the brief at " + addon::path_to_utf8(brief) +
        " before anything else and follow it; do not modify the project until the user confirms.";
    direct.initial_prompt = addon::Initial_prompt_request{text, addon::path_to_utf8(brief)};
    std::vector<addon::Prepare_request> launches{direct};
#ifdef _WIN32
    auto powershell = direct;
    powershell.launcher.runner_key = addon::k_runner_powershell;
    powershell.launcher.script_body = "& " + quote(addon::path_to_utf8(fake)) +
        " --model 'chosen-model' --approval-mode ('plan')";
    powershell.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", powershell.launcher.script_body};
    launches.push_back(powershell);
    const auto script = root / "qwen.ps1";
    std::ofstream(script, std::ios::binary) << "& " << quote(addon::path_to_utf8(fake)) << " @args\nexit $LASTEXITCODE\n";
    auto shim = direct;
    shim.launcher_argv.front() = addon::path_to_utf8(script);
    shim.launcher.script_body = '"' + addon::path_to_utf8(script) +
        "\" --model chosen-model --approval-mode plan";
    launches.push_back(shim);
    auto shell_shim = powershell;
    shell_shim.launcher.script_body = "& " + quote(addon::path_to_utf8(script)) +
        " --model 'chosen-model' --approval-mode ('plan')";
    shell_shim.launcher_argv.back() = shell_shim.launcher.script_body;
    launches.push_back(shell_shim);
    const auto batch = root / "qwen.cmd";
    std::ofstream(batch, std::ios::binary) << "@echo off\r\n\"" << addon::path_to_utf8(fake) << "\" %*\r\n";
    auto batch_launch = direct;
    batch_launch.launcher_argv.front() = addon::path_to_utf8(batch);
    batch_launch.launcher.script_body = '"' + addon::path_to_utf8(batch) +
        "\" --model chosen-model --approval-mode plan";
    launches.push_back(batch_launch);
    auto shell_batch = powershell;
    shell_batch.launcher.script_body = "& " + quote(addon::path_to_utf8(batch)) +
        " --model 'chosen-model' --approval-mode ('plan')";
    shell_batch.launcher_argv.back() = shell_batch.launcher.script_body;
    launches.push_back(shell_batch);
    const std::vector<std::string> overlong_arguments{
        addon::path_to_utf8(batch), "--model", std::string(3700, 'm'),
        "--openai-base-url", std::string(3700, 'u')};
    require(!qwen::platform_command(overlong_arguments, qwen::environment_pairs(environment.effective_environment),
            direct.working_directory),
        "batch shims refuse an invocation beyond cmd.exe's command-line limit");
    auto no_exit = powershell;
    no_exit.launcher_argv.insert(no_exit.launcher_argv.begin() + 1, "-NoExit");
    Context no_exit_context;
    const auto no_exit_patch = prepare(no_exit, no_exit_context);
    require(no_exit_patch && no_exit_patch->argv &&
        std::find(no_exit_patch->argv->begin(), no_exit_patch->argv->end(), "-NoExit") != no_exit_patch->argv->end() &&
        !no_exit_patch->argv->back().ends_with("; exit $LASTEXITCODE"),
        "an explicitly persistent PowerShell runner remains persistent after its native CLI exits");
#endif
    auto literal = direct;
    literal.initial_prompt->text = "Read \"quoted\" data 'single' $VARIABLE %PLACEHOLDER% & | <> ^ ` \\";
    launches.push_back(std::move(literal));
    addon::Contract_failure failure;
    for (const auto& request : launches) {
        Context context;
        fs::remove(root / "help-starts");
        const auto patch = prepare(request, context);
        require(patch && patch->argv && addon::validate_launch_patch(*patch, {}, {}, &failure) &&
            addon::validate_initial_prompt_acknowledgment(request.initial_prompt, *patch, &failure),
            "the host accepts the exact prompt digest and prepared interactive invocation");
        require(fs::file_size(root / "help-starts") == 1,
            "initial prompt preparation must start the selected program for --help exactly once");
        require(patch->account_context &&
            patch->account_context->adapter_account_key == request.account_context->adapter_account_key &&
            patch->account_context->profile_path == request.account_context->profile_path &&
            patch->apply_managed_configuration,
            "initial-prompt preparation retains the selected account and managed hooks");
        const auto output = qwen::probe(*patch->argv, qwen::environment_pairs(environment.effective_environment),
            request.working_directory, 10000, context);
        const auto capture = output ? addon::parse_json(output->output, {}, nullptr) : std::nullopt;
        require(capture && capture->is_object() && capture->find("home")->as_string() == home,
            "the proposed command reaches its native child with the selected profile");
        std::vector<std::string> expected = options;
        expected.insert(expected.end(), {"--include-directories", addon::path_to_utf8(brief.parent_path()),
            "--prompt-interactive=" + request.initial_prompt->text});
        std::vector<std::string> received;
        for (const auto& argument : capture->find("argv")->as_array()) {
            received.push_back(argument.as_string());
        }
        require(received == expected,
            "model, permission policy, narrow brief directory and UTF-8 prompt arrive exactly once");
        auto unprompted = request;
        unprompted.initial_prompt.reset();
        Context ordinary;
        const auto ordinary_patch = prepare(unprompted, ordinary);
        require(ordinary_patch && !ordinary_patch->initial_prompt,
            "ordinary launch does not acknowledge a prompt or grant a brief directory");
        for (const auto& changed_text : std::vector<std::string>{
                " read this brief", "read this brief ", "/clear", "?btw follow up", "exit",
                "read @brief.md", "\"Read this brief\"", "'Read this brief'", "\"", "'",
                "\xEF\xBB\xBFread this brief", "read\xC2\xA0@brief.md", "?btw\xC2\xA0" "follow up"})
        {
            auto changed = request;
            changed.initial_prompt->text = changed_text;
            require_refusal(std::move(changed));
        }
        for (const auto& directory : std::vector<fs::path>{root / "briefs, wider", root / "briefs "}) {
            auto changed = request;
            changed.initial_prompt->referenced_file = addon::path_to_utf8(directory / "brief.md");
            require_refusal(std::move(changed));
        }
    }
    for (const auto& conflict : std::vector<std::vector<std::string>>{
            {"--continue"}, {"--resume", "previous"}, {"-p", "another message"},
            {"--prompt-interactive", "another message"}, {"mcp", "list"},
            {"--model", "--continue"}, {"--agent", "another-agent"}, {"an earlier prompt"}})
    {
        auto changed = direct;
        changed.launcher_argv = {addon::path_to_utf8(fake)};
        changed.launcher.script_body = '"' + addon::path_to_utf8(fake) + '"';
        for (const auto& argument : conflict) {
            changed.launcher_argv.push_back(argument);
            changed.launcher.script_body += " \"" + argument + '"';
        }
        require_refusal(std::move(changed));
    }
    for (const auto* help : {
            "  --prompt-interactive <prompt>\n",
            "  --prompt-interactive-other <prompt>\n  --include-directories <dirs>\n"})
    {
        auto absent_option = direct;
        absent_option.launch_environment->effective_environment.set("QWEN_INITIAL_TEST_HELP", addon::Json_value(help));
        require_refusal(std::move(absent_option));
    }
    fs::remove(root / "help-starts");
    Context cancelled;
    cancelled.m_cancelled = true;
    try {
        (void)prepare(direct, cancelled);
    }
    catch (const std::runtime_error& error) {
        require(std::string_view(error.what()) == "cancelled", "pre-cancelled preparation lost the reason");
    }
    require(cancelled.m_failure_status == addon::Result_status::CANCELLED && !cancelled.m_result &&
        !fs::exists(root / "help-starts"), "pre-cancelled preparation must stop before probing");
#ifndef _WIN32
    const auto modified = fs::last_write_time(fake);
    auto changing_program = direct;
    changing_program.launch_environment->effective_environment.set("QWEN_INITIAL_TEST_TOUCH", addon::Json_value("1"));
    require_refusal(std::move(changing_program));
    require(fs::last_write_time(fake) != modified, "the help fixture must actually change the selected program");
#endif
    auto shell = direct;
    shell.launcher.runner_key = addon::k_runner_bash;
    shell.launcher_argv = {"bash", "-c", shell.launcher.script_body};
    require_refusal(std::move(shell));
}

int run(const std::vector<std::string>& arguments)
{
    if (qwen::base_name(arguments.front()) == "qwen") {
        if (arguments.size() == 2 && arguments[1] == "--help") {
            std::ofstream("help-starts", std::ios::binary | std::ios::app).put('x');
#ifndef _WIN32
            if (std::getenv("QWEN_INITIAL_TEST_TOUCH")) {
                const auto program = addon::path_from_utf8(arguments.front());
                fs::last_write_time(program, fs::last_write_time(program) - std::chrono::seconds(5));
            }
#endif
            const char* help = std::getenv("QWEN_INITIAL_TEST_HELP");
            std::cout << (help ? help : "  --prompt-interactive <prompt>\n  --include-directories <dirs>\n");
            return 0;
        }
        const auto home = qwen::environment_value(addon::current_process_environment(), "QWEN_HOME");
        auto capture = addon::Json_value::object();
        capture.set("home", addon::Json_value(home));
        auto argv = addon::Json_value::array();
        for (auto argument = arguments.begin() + 1; argument != arguments.end(); ++argument) {
            argv.push_back(addon::Json_value(*argument));
        }
        capture.set("argv", std::move(argv));
        std::cout << addon::serialize_json(capture);
        return 0;
    }
    try {
        initial_prompt_delivery(fs::absolute(addon::path_from_utf8(arguments.front())));
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(addon::path_to_utf8(fs::path(argv[index])));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
