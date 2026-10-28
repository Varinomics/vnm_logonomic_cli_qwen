#include "qwen_launcher.h"
#include "qwen_json.h"
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>
namespace json_util = logonomic::cli_addon::json;

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
        const bool command = (shell == "cmd" && flag == "/c") ||
            ((shell == "bash" || shell == "sh") && flag == "-c");
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
    const auto argv = addon::literal_argv(script);
    return !argv.empty() && base_name(argv.front()) == "qwen";
}

bool plain_session_options(const std::vector<std::string>& argv)
{
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& option = argv[index];
        if (option == "--debug" || option == "-d" || option == "--yolo" || option == "-y" ||
            option == "--yolo=true" || option == "--yolo=false")
        {
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

bool recognize_launcher(const addon::Launcher_request& launcher, const Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context)
{
    if (launcher.runner_key == addon::k_runner_powershell) {
#ifdef _WIN32
        return inspect_powershell_launcher(launcher, environment, working_directory, context).has_value();
#else
        (void)environment;
        (void)working_directory;
        (void)context;
        return false;
#endif
    }
    return recognize_launcher(launcher.script_body);
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

#ifdef _WIN32
namespace {

bool cmd_arguments_fit(const std::vector<std::string>& command)
{
    // cmd.exe's 8191-character limit also applies after an npm batch shim
    // expands its driver and entrypoint paths. Reserve their normal MAX_PATH
    // allowance, as the Claude integration does. UTF-8 bytes bound UTF-16 units.
    constexpr std::size_t k_cmd_line_characters   = 8191;
    constexpr std::size_t k_cmd_program_allowance = 1024;
    std::size_t characters = k_cmd_program_allowance;
    for (auto argument = command.begin() + 1; argument != command.end(); ++argument) {
        const auto kept = argument->find_last_not_of('\\');
        characters += 3 + argument->size() +
            (kept == std::string::npos ? argument->size() : argument->size() - kept - 1);
    }
    return characters <= k_cmd_line_characters;
}

} // namespace
#endif

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
        if (extension != ".ps1" && !cmd_arguments_fit(argv)) {
            return std::nullopt;
        }
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

#ifdef _WIN32
namespace {

std::string powershell_quote(std::string_view value)
{
    std::string result = "'";
    for (const char character : value) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + "'";
}

constexpr std::string_view k_powershell_analysis = R"PS(
try {
    $source = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($SourceBase64))
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0 -or $ast.EndBlock.Statements.Count -ne 1) { throw 'syntax' }
    $pipeline = $ast.EndBlock.Statements[0]
    if ($pipeline -isnot [System.Management.Automation.Language.PipelineAst] -or
        $pipeline.PipelineElements.Count -ne 1 -or $pipeline.Background) { throw 'pipeline' }
    $command = $pipeline.PipelineElements[0]
    if ($command -isnot [System.Management.Automation.Language.CommandAst] -or
        $command.Redirections.Count -ne 0 -or $command.CommandElements.Count -eq 0 -or
        $command.InvocationOperator -notin @('Unknown', 'Ampersand')) { throw 'command' }
    function LiteralValue($node) {
        if ($node -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
            return $node.Value
        }
        if ($node -is [System.Management.Automation.Language.CommandParameterAst] -and $null -eq $node.Argument) {
            return $node.Extent.Text
        }
        if ($node -is [System.Management.Automation.Language.ParenExpressionAst] -and
            $node.Pipeline.PipelineElements.Count -eq 1 -and
            $node.Pipeline.PipelineElements[0] -is [System.Management.Automation.Language.CommandExpressionAst] -and
            $node.Pipeline.PipelineElements[0].Expression -is [System.Management.Automation.Language.StringConstantExpressionAst]) {
            return $node.Pipeline.PipelineElements[0].Expression.Value
        }
        throw 'dynamic'
    }
    $head = $command.CommandElements[0]
    if ($head -isnot [System.Management.Automation.Language.StringConstantExpressionAst] -or
        ($command.InvocationOperator -eq 'Unknown' -and $head.StringConstantType -ne 'BareWord')) { throw 'head' }
    $headValue = LiteralValue $head
    if ([IO.Path]::GetFileName($headValue) -notmatch '^qwen(?:\.(?:exe|cmd|bat|ps1))?$') { throw 'name' }
    $arguments = @()
    foreach ($item in $command.CommandElements | Select-Object -Skip 1) {
        $arguments += [pscustomobject]@{ source = $item.Extent.Text; value = [string](LiteralValue $item) }
    }
    $matches = @(Get-Command -Name ([Management.Automation.WildcardPattern]::Escape($headValue)) -ErrorAction Stop)
    if ($matches.Count -ne 1 -or $matches[0].CommandType -notin @('Application', 'ExternalScript')) { throw 'resolve' }
    $payload = [ordered]@{ path = $matches[0].Path; args = @($arguments) }
    $json = ConvertTo-Json -InputObject $payload -Compress -Depth 4
    [Console]::Out.WriteLine('QWEN_SELECTION:' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json)))
}
catch { exit 3 }
)PS";

} // namespace

std::optional<Powershell_selection> inspect_powershell_launcher(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory,
    addon::Invocation_context& context)
{
    if (launcher.runner_key != addon::k_runner_powershell) {
        return std::nullopt;
    }
    std::vector<std::string> expected{"powershell.exe"};
    if (argv.size() == 5) {
        expected.push_back("-NoExit");
    }
    expected.insert(expected.end(), {"-NoLogo", "-Command", launcher.script_body});
    if (argv != expected) {
        return std::nullopt;
    }
    auto selection = inspect_powershell_launcher(launcher, environment, working_directory, context);
    if (selection) {
        selection->no_exit = argv.size() == 5;
    }
    return selection;
}

std::optional<Powershell_selection> inspect_powershell_launcher(
    const addon::Launcher_request& launcher, const Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context)
{
    if (launcher.runner_key != addon::k_runner_powershell) {
        return std::nullopt;
    }
    const auto shell = resolve_program("powershell.exe", environment, working_directory);
    if (!shell || !native_image(*shell)) {
        return std::nullopt;
    }
    const std::string analysis = "$SourceBase64='" + addon::encode_base64(launcher.script_body) + "'\n" +
        std::string(k_powershell_analysis);
    const auto output = probe({path_string(*shell), "-NoLogo", "-NonInteractive", "-Command", analysis},
        environment, working_directory, 10000, context);
    if (!output) {
        return std::nullopt;
    }
    constexpr std::string_view marker = "QWEN_SELECTION:";
    const auto marker_position = output->output.rfind(marker);
    if (marker_position == std::string::npos) {
        return std::nullopt;
    }
    const auto decoded = addon::decode_base64(trim(output->output.substr(marker_position + marker.size())));
    const auto result = decoded ? json_util::parse(*decoded) : std::nullopt;
    if (!result || !json_util::exact_keys(*result, {"path", "args"}) ||
        !json_util::bounded_string(json_util::member(*result, "path"), 4096) ||
        !json_util::member(*result, "args").is_array())
    {
        return std::nullopt;
    }
    Powershell_selection selection;
    selection.shell = path_string(*shell);
    selection.program = json_util::text(*result, "path");
    if (base_name(selection.program) != "qwen") {
        return std::nullopt;
    }
    selection.option_values.push_back(selection.program);
    for (const auto& argument : json_util::member(*result, "args").as_array()) {
        if (!json_util::exact_keys(argument, {"source", "value"}) ||
            !json_util::bounded_string(json_util::member(argument, "source"), 4096) ||
            !json_util::bounded_string(json_util::member(argument, "value"), 4096, true))
        {
            return std::nullopt;
        }
        selection.argument_expressions.push_back(json_util::text(argument, "source"));
        selection.option_values.push_back(json_util::text(argument, "value"));
    }
    return plain_session_options(selection.option_values) ? std::optional(std::move(selection)) : std::nullopt;
}

std::optional<std::vector<std::string>> powershell_session_command(
    const Powershell_selection& selection, const std::vector<std::string>& arguments)
{
    const auto extension = lower(addon::path_from_utf8(selection.program).extension().string());
    if (extension == ".cmd" || extension == ".bat") {
        auto command = selection.option_values;
        command.insert(command.end(), arguments.begin(), arguments.end());
        if (!cmd_arguments_fit(command)) {
            return std::nullopt;
        }
    }
    std::string script = "& " + powershell_quote(selection.program);
    for (const auto& expression : selection.argument_expressions) {
        script += " " + expression;
    }
    for (const auto& argument : arguments) {
        script += " " + powershell_quote(argument);
    }
    if (!selection.no_exit) {
        script += "; exit $LASTEXITCODE";
    }
    std::vector<std::string> command{selection.shell};
    if (selection.no_exit) {
        command.push_back("-NoExit");
    }
    command.insert(command.end(), {"-NoLogo", "-Command", std::move(script)});
    return command;
}
#endif

std::optional<std::vector<std::string>> selected_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory)
{
    if (launcher.runner_key != "direct" || !recognize_launcher(launcher.script_body) || argv.empty())
    {
        return std::nullopt;
    }
    const auto literal = addon::literal_argv(launcher.script_body);
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

bool selected_program_supports(
    std::string_view program, const Environment& environment,
    std::string_view working_directory, std::string_view required_option,
    addon::Invocation_context& context)
{
    const auto selected = addon::path_from_utf8(program);
    const auto probe_command = platform_command({std::string(program), "--help"}, environment, working_directory);
    if (!probe_command) {
        return false;
    }
    std::error_code error;
    const auto size = fs::file_size(selected, error);
    if (error) {
        return false;
    }
    const auto modified = fs::last_write_time(selected, error);
    if (error) {
        return false;
    }
    const auto output = probe(*probe_command, environment, working_directory, 10000, context);
    if (!output || fs::file_size(selected, error) != size || error ||
        fs::last_write_time(selected, error) != modified || error)
    {
        return false;
    }
    // Capability evidence comes from the selected executable, never a ceiling
    // on a moving vendor release number.
    const std::string help = output->output + '\n' + output->errors;
    for (auto option = help.find(required_option); option != std::string::npos;
         option = help.find(required_option, option + 1))
    {
        if ((option > 0 && help[option - 1] != ' ' && help[option - 1] != '\t') ||
            (option + required_option.size() < help.size() &&
                (std::isalnum(static_cast<unsigned char>(help[option + required_option.size()])) ||
                    help[option + required_option.size()] == '-' || help[option + required_option.size()] == '_')))
        {
            continue;
        }
        return true;
    }
    return false;
}

std::optional<std::vector<std::string>> verified_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory,
    std::string_view required_option, addon::Invocation_context& context)
{
    auto command = selected_command(launcher, argv, environment, working_directory);
    if (!command || !selected_program_supports(command->front(), environment,
        working_directory, required_option, context))
    {
        return std::nullopt;
    }
    return command;
}

} // namespace qwen
