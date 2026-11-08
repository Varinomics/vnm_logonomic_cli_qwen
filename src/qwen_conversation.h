#pragma once

#include <logonomic_cli_addon/conversation_export.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace qwen {

struct Conversation_records
{
    std::vector<logonomic::cli_addon::Json_value> records;
    std::map<std::string, std::vector<std::size_t>> fragments;
    std::vector<std::string> chain;
    logonomic::cli_addon::Conversation_snapshot snapshot;
};

// Read the retained native branch without opening a conversation export.
std::optional<Conversation_records> read_conversation_records(
    const std::filesystem::path& source, std::string_view session_id,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

bool append_conversation_record(const logonomic::cli_addon::Json_value& row,
    logonomic::cli_addon::Conversation_message& message,
    logonomic::cli_addon::Contract_failure* failure);

std::optional<logonomic::cli_addon::Conversation_export_result> export_conversation(
    const logonomic::cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

} // namespace qwen
