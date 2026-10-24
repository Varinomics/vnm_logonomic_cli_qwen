#include <logonomic_cli_addon/package_source.h>
#include "qwen_profile.h"

#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <stdexcept>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

} // namespace

std::string account_key(const fs::path& home)
{
    auto normalized = fs::absolute(home).lexically_normal();
#ifdef _WIN32
    const auto source = normalized.native();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) throw std::runtime_error("qwen_profile_normalization_failed");
    std::wstring folded(static_cast<std::size_t>(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source.data(), static_cast<int>(source.size()),
            folded.data(), count, nullptr, nullptr, 0) != count)
    {
        throw std::runtime_error("qwen_profile_normalization_failed");
    }
    normalized = fs::path(folded);
#endif
    return "profile-" + addon::to_hex(addon::Sha256::digest(path_string(normalized)));
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
    result.environment = environment_pairs(environment.effective_environment);
    return result;
}

resolved_profile_t profile_for_context(const addon::Session_launch_context& context)
{
    if (!context.launch_environment) throw std::runtime_error("launch_environment_unavailable");
    if (!context.launch_environment->bootstrap) throw std::runtime_error("environment_bootstrap_unavailable");
    auto profile = profile_from_environment(*context.launch_environment);
    if (!context.account_context) return profile;
    if (!context.account_context->profile_path ||
        account_key(profile.home) != context.account_context->adapter_account_key)
    {
        throw std::runtime_error("account_profile_mismatch");
    }
#ifdef _WIN32
    const bool profile_path_matches =
        account_key(addon::path_from_utf8(*context.account_context->profile_path)) ==
        context.account_context->adapter_account_key;
#else
    const bool profile_path_matches = path_string(profile.home) == *context.account_context->profile_path;
#endif
    if (!profile_path_matches)
    {
        throw std::runtime_error("account_profile_mismatch");
    }
    return profile;
}

} // namespace qwen
