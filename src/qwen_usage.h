#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace qwen {

struct Usage_limits
{
    // Pure scan bounds; the runtime uses these defaults. No provider files or
    // cache artifacts are written when a bound or cancellation stops a scan.
    std::uint64_t source_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t source_entries = 1048576;
    std::uint64_t records = 1048576;
    std::uint64_t result_bytes = 1024ULL * 1024 * 1024;
};

std::vector<logonomic::cli_addon::Usage_record> scan_usage(
    const std::filesystem::path& home,
    logonomic::cli_addon::Invocation_context& context, Usage_limits limits = {});
void handle_usage(logonomic::cli_addon::Invocation_context& context);

} // namespace qwen
