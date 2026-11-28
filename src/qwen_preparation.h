#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace qwen {

void handle_preparation(logonomic::cli_addon::Invocation_context& context);
void handle_authoring(logonomic::cli_addon::Invocation_context& context);

} // namespace qwen
