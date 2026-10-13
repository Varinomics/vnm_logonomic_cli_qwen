#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace qwen {

inline constexpr const char* k_wake_binding_environment = "QWEN_LOGONOMIC_WAKE_BINDING";

struct Peer_target
{
    std::filesystem::path record;
    std::string           session_id;
    std::string           socket_path;
    std::string           token;
    std::string           process_start;
    std::string           launch_token;
    int                   pid = 0;
};

struct Peer_result
{
    std::string status;
    bool        written = false;
};

// Qwen's published cross-session protocol owns registry and receipt semantics.
// A token never leaves this native transport or enters a capability result.
std::optional<Peer_target> find_peer(
    const std::filesystem::path& profile, std::string_view session_id, std::string_view launch_token);
bool peer_reachable(const Peer_target& target);
Peer_result send_peer(
    const Peer_target& target,
    std::string_view message_id,
    std::string_view notification,
    const std::function<bool()>& cancelled);

} // namespace qwen
