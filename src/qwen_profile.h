#pragma once

#include "qwen_json.h"
#include "qwen_launcher.h"

#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <string_view>

namespace qwen {

struct resolved_profile_t
{
    std::filesystem::path home;
    // Explicit environment runtime override, otherwise home. This is not an
    // instruction to override Qwen's own settings-based runtimeDir selection.
    Environment           environment;
};

std::string account_key(const std::filesystem::path& home);
std::filesystem::path profile_directory(const std::filesystem::path& path);
bool matches_profile_key(const std::filesystem::path& home, const std::filesystem::path& retained, std::string_view key);
#ifndef _WIN32
std::string historical_posix_account_key(const std::filesystem::path& path);
#endif
std::filesystem::path native_io_path(const std::filesystem::path& path);
resolved_profile_t profile_from_environment(const logonomic::cli_addon::Launch_environment& environment);
resolved_profile_t profile_for_context(const logonomic::cli_addon::Session_launch_context& context);

} // namespace qwen
