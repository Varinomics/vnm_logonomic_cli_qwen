#include <logonomic_cli_addon/package_source.h>
#include "qwen_profile.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <stdexcept>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

std::string profile_key(std::string_view identity)
{
    return "profile-" + addon::to_hex(addon::Sha256::digest(identity));
}

bool matches_account(const fs::path& home, const addon::Account_context& grant)
{
    if (!grant.profile_path) return false;
    const auto retained = addon::path_from_utf8(*grant.profile_path);
    if (!retained.is_absolute()) return false;
#ifdef _WIN32
    const auto selected = fs::absolute(home).lexically_normal();
    const auto original = fs::absolute(retained).lexically_normal();
    if (grant.adapter_account_key != account_key(home)) {
        const auto historical = addon::historical_account_profile_identity(original, selected);
        return historical && grant.adapter_account_key == profile_key(*historical);
    }
    return addon::same_native_account_profile(original, selected);
#else
    return retained.lexically_normal() == home && grant.adapter_account_key == account_key(home);
#endif
}

} // namespace

fs::path native_io_path(const fs::path& path)
{
#ifdef _WIN32
    // The extended prefix disables Win32's ordinary dot-segment resolution.
    return addon::usage_cache_io_path(path.lexically_normal());
#else
    return path;
#endif
}

std::string account_key(const fs::path& home)
{
    const auto identity = addon::native_account_profile_identity(fs::absolute(home).lexically_normal());
    if (!identity) throw std::runtime_error("qwen_profile_normalization_failed");
    return profile_key(*identity);
}

resolved_profile_t profile_from_environment(const addon::Launch_environment& environment)
{
    if (!environment.bootstrap || environment.bootstrap->resolved_home.empty()) {
        throw std::runtime_error("environment_bootstrap_unavailable");
    }
    resolved_profile_t result;
    result.home = addon::path_from_utf8(environment.bootstrap->resolved_home);
    if (!result.home.is_absolute()) {
        throw std::runtime_error("qwen_profile_normalization_unavailable");
    }
#ifndef _WIN32
    // Qwen uses Node's path.join to select files inside this directory. Dot
    // segments are removed before the kernel follows any directory symlinks.
    result.home = result.home.lexically_normal();
#endif
    result.environment = environment_pairs(environment.effective_environment);
    return result;
}

resolved_profile_t profile_for_context(const addon::Session_launch_context& context)
{
    if (!context.launch_environment) throw std::runtime_error("launch_environment_unavailable");
    if (!context.launch_environment->bootstrap) throw std::runtime_error("environment_bootstrap_unavailable");
    auto profile = profile_from_environment(*context.launch_environment);
    if (context.account_context && !matches_account(profile.home, *context.account_context)) {
        throw std::runtime_error("account_profile_mismatch");
    }
    return profile;
}

} // namespace qwen
