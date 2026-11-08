#pragma once

#include "qwen_json.h"

#include <logonomic_cli_addon/session_contracts.h>

#include <functional>
#include <string>
#include <string_view>

namespace qwen {

inline constexpr std::string_view k_profile_home_environment = "QWEN_LOGONOMIC_PROFILE_HOME";

struct Resume_inspection
{
    logonomic::cli_addon::Resume_availability availability =
        logonomic::cli_addon::Resume_availability::UNKNOWN;
    logonomic::cli_addon::Resume_content content = logonomic::cli_addon::Resume_content::UNKNOWN;
    bool preparation_allowed = false;
    std::string preparation_failure;
};

bool valid_resume_record(const Json& record);
Json make_resume_record(std::string_view session_id, const Json& body);

Resume_inspection inspect_resume(const logonomic::cli_addon::Session_launch_context& context,
    const Json& record, const std::function<bool()>& cancelled = {});

} // namespace qwen
