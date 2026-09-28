#include "qwen_accounts.h"
#include "qwen_launcher.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/accounts.h>

#include <stdexcept>
#include <system_error>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

addon::Account_record account(const resolved_profile_t& profile)
{
    addon::Account_record result;
    result.adapter_account_key = account_key(profile.home);
    result.display_name = addon::account_display_name(path_string(profile.home));
    result.profile_path = path_string(profile.home);
    // A profile that cannot be checked, such as one on an unreachable share, is
    // an unavailable account: it must not fail the discovery of every account.
    std::error_code error;
    result.status = fs::is_directory(profile.home, error)
        ? addon::Account_status::AVAILABLE
        : addon::Account_status::UNAVAILABLE;
    return result;
}

} // namespace

void handle_accounts(addon::Invocation_context& context)
{
    if (context.request().operation != "discover" && context.request().operation != "resolve") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!input) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    if (!input->launch_environment.bootstrap) {
        context.fail(addon::Result_status::UNAVAILABLE, "environment_bootstrap_unavailable");
        return;
    }
    if (!recognize_launcher(input->launcher,
        environment_pairs(input->launch_environment.effective_environment),
        input->normalization_directory, context)) {
        context.succeed(context.request().operation == "discover" ? addon::encode_accounts({}) : addon::encode_account_resolution({}));
        return;
    }
    const auto profile = profile_from_environment(input->launch_environment);
    if (context.request().operation == "resolve") {
        context.succeed(addon::encode_account_resolution({account_key(profile.home)}));
        return;
    }
    if (context.request().operation != "discover") { context.reject_unknown_operation(); return; }
    context.succeed(addon::encode_accounts({account(profile)}));
}

} // namespace qwen
