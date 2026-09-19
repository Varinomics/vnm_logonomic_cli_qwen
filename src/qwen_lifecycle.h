#pragma once

#include <logonomic_cli_addon/helper_ingress.h>

namespace qwen {

logonomic::cli_addon::Classify_event_result classify_event(
    const logonomic::cli_addon::Classify_event_request& request);

} // namespace qwen
