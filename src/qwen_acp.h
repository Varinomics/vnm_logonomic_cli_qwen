#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <string>
#include <string_view>

namespace qwen {

std::string initial_acp_input(const logonomic::cli_addon::Structured_prepare_request& request);
std::string initial_acp_input(const logonomic::cli_addon::Delegated_prepare_request& request);
std::string initial_acp_state(std::string_view prompt);
logonomic::cli_addon::Structured_inspect_result inspect_acp(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace qwen
