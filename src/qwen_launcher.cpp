#include "qwen_launcher.h"
#include <logonomic_cli_addon/package_source.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>
namespace qwen {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
std::string path_string(const fs::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

std::vector<std::string> literal_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (const char character : script) {
        if (character == '"') {
            if (++quotes == 3) { current += '"'; quotes = 0; }
            continue;
        }
        if (quotes == 1) quoted = !quoted;
        quotes = 0;
        if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else current += character;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

bool is_qwen_invocation(const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        return false;
    }
    if (base_name(argv.front()) == "qwen") {
        return true;
    }
    const auto shell = base_name(argv.front());
    for (std::size_t index = 1; index + 1 < argv.size(); ++index) {
        const auto flag = lower(argv[index]);
        const bool command = ((shell == "powershell" || shell == "pwsh") && flag == "-command") ||
            (shell == "cmd" && flag == "/c") || ((shell == "bash" || shell == "sh") && flag == "-c");
        if (command && index + 2 == argv.size()) {
            return recognize_launcher(argv.back());
        }
    }
    return false;
}

bool recognize_launcher(std::string_view script)
{
    if (script.find_first_of(";&|<>$`%\r\n") != std::string_view::npos) {
        return false;
    }
    const auto argv = literal_argv(script);
    return !argv.empty() && base_name(argv.front()) == "qwen";
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    const auto environment = environment_pairs(request.ambient_environment);
    const auto program = resolve_program("qwen", environment, request.default_home_fallback);
    if (!program) {
        return std::nullopt;
    }
    addon::Launcher_candidate result;
    result.candidate_id = "qwen";
    result.display_name = "Qwen Code";
    // npm installs a qwen symlink to a JavaScript entry point. Retain the
    // command identity; later preparation resolves the same frozen PATH.
    result.script_body = "qwen";
    result.runner_key = "direct";
    result.is_default = true;
    return result;
}

std::optional<std::vector<std::string>> platform_command(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory)
{
    if (argv.empty()) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto extension = lower(addon::path_from_utf8(argv.front()).extension().string());
    if (extension == ".cmd" || extension == ".bat" || extension == ".ps1") {
        const auto shell = resolve_program("powershell.exe", environment, working_directory);
        if (!shell || !native_image(*shell)) {
            return std::nullopt;
        }
        std::string script = "&";
        for (const auto& argument : argv) {
            // cmd wrappers perform a second expansion pass. Do not feed that
            // pass shell syntax through an otherwise literal process contract.
            if (extension != ".ps1" && argument.find_first_of("\"%&|<>^!()\r\n") != std::string::npos) {
                return std::nullopt;
            }
            script += " '";
            for (const auto character : argument) {
                script += character;
                if (character == '\'') {
                    script += '\'';
                }
            }
            script += '\'';
        }
        script += "; exit $LASTEXITCODE";
        return std::vector<std::string>{path_string(*shell), "-NoLogo", "-NoProfile",
            "-NonInteractive", "-Command", std::move(script)};
    }
    if (!native_image(addon::path_from_utf8(argv.front()))) {
        return std::nullopt;
    }
#else
    (void)environment;
    (void)working_directory;
#endif
    return argv;
}

std::optional<std::vector<std::string>> selected_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory)
{
    if (launcher.runner_key != "direct" || !recognize_launcher(launcher.script_body) || argv.empty())
    {
        return std::nullopt;
    }
    const auto literal = literal_argv(launcher.script_body);
    if (literal.size() != argv.size() ||
        !std::equal(literal.begin() + 1, literal.end(), argv.begin() + 1))
    {
        return std::nullopt;
    }
    const auto selected = resolve_program(argv.front(), environment, working_directory);
    const auto authored = resolve_program(literal.front(), environment, working_directory);
    if (!selected || !authored || !equal_name(path_string(*selected), path_string(*authored))) {
        return std::nullopt;
    }
    auto command = argv;
    command.front() = path_string(*selected);
    return command;
}

std::optional<std::vector<std::string>> verified_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory,
    std::string_view required_option, addon::Invocation_context& context)
{
    const auto selected_argv = selected_command(launcher, argv, environment, working_directory);
    if (!selected_argv) {
        return std::nullopt;
    }
    const auto& command = *selected_argv;
    const auto selected = addon::path_from_utf8(command.front());
    const auto probe_command = platform_command({command.front(), "--help"}, environment, working_directory);
    if (!probe_command) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = fs::file_size(selected, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = fs::last_write_time(selected, error);
    if (error) {
        return std::nullopt;
    }
    const auto output = probe(*probe_command, environment, working_directory, 10000, context);
    if (!output || fs::file_size(selected, error) != size || error ||
        fs::last_write_time(selected, error) != modified || error)
    {
        return std::nullopt;
    }
    // Capability evidence comes from the selected executable, never a ceiling
    // on a moving vendor release number.
    const std::string help = output->output + '\n' + output->errors;
    const auto option = help.find(required_option);
    if (option == std::string::npos ||
        (option > 0 && help[option - 1] != ' ' && help[option - 1] != '\t') ||
        (option + required_option.size() < help.size() &&
            std::isalnum(static_cast<unsigned char>(help[option + required_option.size()]))))
    {
        return std::nullopt;
    }
    return command;
}

} // namespace qwen
