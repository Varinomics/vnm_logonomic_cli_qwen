#pragma once

#include <logonomic_cli_addon/session_contracts.h>

#include <string>
#include <string_view>
#include <vector>

namespace qwen {

// The host relay writes `nonce` to `path` once the Task server's catalog
// succeeded; `parent_artifact` makes host materialization create its folder.
struct Task_server_receipt
{
    std::string server_id;
    std::string path;
    std::string nonce;
    std::string parent_artifact;
    std::vector<std::string> tool_names;
};

std::vector<Task_server_receipt> task_server_receipts(const logonomic::cli_addon::Delegated_prepare_request& request);
std::string initial_acp_input(const logonomic::cli_addon::Structured_prepare_request& request);
std::string initial_acp_input(const logonomic::cli_addon::Delegated_prepare_request& request,
    const std::vector<Task_server_receipt>& receipts);
std::string initial_acp_state(std::string_view prompt, const std::vector<Task_server_receipt>& receipts = {});
logonomic::cli_addon::Structured_inspect_result inspect_acp(
    const logonomic::cli_addon::Structured_inspection& inspection);

} // namespace qwen
