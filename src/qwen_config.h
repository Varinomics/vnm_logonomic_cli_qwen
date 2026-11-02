#pragma once

#include "qwen_json.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace qwen {

class Hooks_disabled_by_user final : public std::runtime_error
{
public:
    Hooks_disabled_by_user() : std::runtime_error("hooks_disabled_by_user") {}
};

// Pure transformation of the host-granted settings snapshot. No paths or I/O.
std::string updated_config(std::string_view settings);
Json parse_settings(std::string_view settings);
Json hook_entry(std::string_view event);
// An explicit user opt-out raises Hooks_disabled_by_user, as in updated_config().
bool has_managed_hooks(std::string_view settings);

} // namespace qwen
