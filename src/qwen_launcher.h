#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qwen {

using logonomic::cli_addon::Environment;
using logonomic::cli_addon::Probe_output;
using logonomic::cli_addon::trim;
using logonomic::cli_addon::lower;
using logonomic::cli_addon::unquote;
using logonomic::cli_addon::base_name;
using logonomic::cli_addon::environment_value;
using logonomic::cli_addon::set_environment;
using logonomic::cli_addon::environment_pairs;
using logonomic::cli_addon::regular_file;
using logonomic::cli_addon::resolve_program;
using logonomic::cli_addon::native_image;
using logonomic::cli_addon::probe;

std::string path_string(const std::filesystem::path& path);
std::vector<std::string> literal_argv(std::string_view script);
#ifdef _WIN32
struct Powershell_selection
{
    std::string shell;
    std::string program;
    std::vector<std::string> option_values;
    std::vector<std::string> argument_expressions;
};

std::optional<Powershell_selection> inspect_powershell_launcher(
    const logonomic::cli_addon::Launcher_request& launcher,
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context);
std::optional<Powershell_selection> inspect_powershell_launcher(
    const logonomic::cli_addon::Launcher_request& launcher, const Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context);
std::vector<std::string> powershell_resume_command(
    const Powershell_selection& selection, std::string_view session);
#endif
bool is_qwen_invocation(const std::vector<std::string>& argv);
bool recognize_launcher(std::string_view script);
bool recognize_launcher(const logonomic::cli_addon::Launcher_request& launcher,
    const Environment& environment, std::string_view working_directory,
    logonomic::cli_addon::Invocation_context& context);
bool plain_session_options(const std::vector<std::string>& argv);
std::optional<logonomic::cli_addon::Launcher_candidate> discover_launcher(
    const logonomic::cli_addon::Discover_request& request);
std::optional<std::vector<std::string>> selected_command(
    const logonomic::cli_addon::Launcher_request& launcher,
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory);

// Returns the selected launcher itself, including its shim when applicable.
// Callers append protocol flags before making the platform command.
std::optional<std::vector<std::string>> verified_command(
    const logonomic::cli_addon::Launcher_request& launcher,
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, std::string_view required_option,
    logonomic::cli_addon::Invocation_context& context);
bool selected_program_supports(
    std::string_view program, const Environment& environment,
    std::string_view working_directory, std::string_view required_option,
    logonomic::cli_addon::Invocation_context& context);
std::optional<std::vector<std::string>> platform_command(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory);

} // namespace qwen
