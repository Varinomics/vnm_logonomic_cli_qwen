#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qwen {

using Environment = std::vector<std::pair<std::string, std::string>>;

struct Probe_output
{
    std::string output;
    std::string errors;
};

std::string trim(std::string_view value);
std::string lower(std::string value);
std::string unquote(std::string_view value);
std::string base_name(std::string_view value);
std::string path_string(const std::filesystem::path& path);
std::string environment_value(const Environment& environment, std::string_view name);
void set_environment(Environment& environment, std::string name, std::string value);
Environment environment_pairs(const logonomic::cli_addon::Json_value& value);
std::optional<std::filesystem::path> regular_file(const std::filesystem::path& path);
std::optional<std::filesystem::path> resolve_program(
    std::string_view program, const Environment& environment, std::string_view working_directory);
bool native_image(const std::filesystem::path& path);
std::optional<Probe_output> probe(const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, logonomic::cli_addon::Invocation_context& context);
std::vector<std::string> literal_argv(std::string_view script);
bool is_qwen_invocation(const std::vector<std::string>& argv);
bool recognize_launcher(std::string_view script);
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
std::optional<std::vector<std::string>> platform_command(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory);

} // namespace qwen
