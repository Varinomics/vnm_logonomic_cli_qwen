#pragma once

#include <logonomic_cli_addon/conversation_export.h>

#include <functional>
#include <optional>

namespace qwen {

std::optional<logonomic::cli_addon::Conversation_export_result> export_conversation(
    const logonomic::cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

} // namespace qwen
