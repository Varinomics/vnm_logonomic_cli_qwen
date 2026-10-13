#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/session_wake.h>

namespace qwen {

logonomic::cli_addon::Session_wake_preparation prepare_session_wake(
    const logonomic::cli_addon::Session_launch_context& context);
void handle_session_wake(logonomic::cli_addon::Invocation_context& context);

} // namespace qwen
