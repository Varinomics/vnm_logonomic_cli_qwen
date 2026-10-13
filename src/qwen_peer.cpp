#include "qwen_peer.h"

#include "qwen_json.h"

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

#ifdef __linux__

using Clock = std::chrono::steady_clock;
constexpr auto k_exchange_timeout = std::chrono::seconds(3);
constexpr std::size_t k_record_limit = 32768;

class Descriptor
{
public:
    explicit Descriptor(int value = -1) : m_value(value) {}
    ~Descriptor() { if (m_value >= 0) close(m_value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : m_value(std::exchange(other.m_value, -1)) {}
    int get() const { return m_value; }

private:
    int m_value;
};

std::string read_file(const fs::path& path)
{
    const Descriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat info{};
    if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size > static_cast<off_t>(k_record_limit))
    {
        return {};
    }
    std::array<char, k_record_limit + 1> buffer{};
    std::size_t size = 0;
    while (size < buffer.size()) {
        const auto count = read(file.get(), buffer.data() + size, buffer.size() - size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return {};
        }
        if (count == 0) {
            return std::string(buffer.data(), size);
        }
        size += static_cast<std::size_t>(count);
    }
    return {};
}

std::string process_start(int pid)
{
    auto boot_id = read_file("/proc/sys/kernel/random/boot_id");
    const auto stat = read_file("/proc/" + std::to_string(pid) + "/stat");
    const auto end = stat.rfind(')');
    if (end == std::string::npos || boot_id.empty()) {
        return {};
    }
    while (!boot_id.empty() && (boot_id.back() == '\n' || boot_id.back() == '\r')) {
        boot_id.pop_back();
    }
    std::istringstream fields(stat.substr(end + 1));
    std::string ticks;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> ticks)) {
            return {};
        }
    }
    return boot_id + ":" + ticks;
}

bool hex_string(std::string_view value, std::size_t size)
{
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool process_has_binding(int pid, std::string_view launch_token)
{
    if (!hex_string(launch_token, 32)) {
        return false;
    }
    const auto environment = read_file("/proc/" + std::to_string(pid) + "/environ");
    const auto expected = std::string(k_wake_binding_environment) + "=" + std::string(launch_token);
    std::size_t offset = 0;
    while (offset < environment.size()) {
        const auto end = environment.find('\0', offset);
        if (end == std::string::npos) {
            return false;
        }
        if (std::string_view(environment).substr(offset, end - offset) == expected) {
            return true;
        }
        offset = end + 1;
    }
    return false;
}

std::optional<Peer_target> read_peer(const fs::path& path, std::string_view session_id, std::string_view launch_token)
{
    const auto parsed = parse(read_file(path));
    if (!parsed || !member(*parsed, "schemaVersion").is_integer() ||
        member(*parsed, "schemaVersion").as_integer() != 1 ||
        text(*parsed, "sessionId") != session_id || !member(*parsed, "pid").is_integer() ||
        !member(*parsed, "pidNs").is_integer())
    {
        return std::nullopt;
    }
    const auto pid = member(*parsed, "pid").as_integer();
    if (pid <= 0 || pid > INT_MAX) {
        return std::nullopt;
    }
    const auto prefix = std::to_string(pid);
    const auto name = path.filename().string();
    const bool bare = name == prefix + ".json";
    const bool suffixed = name.size() == prefix.size() + 14 && name.starts_with(prefix + "-") &&
        name.ends_with(".json") && hex_string(std::string_view(name).substr(prefix.size() + 1, 8), 8);
    struct stat pid_namespace{};
    if ((!bare && !suffixed) || stat("/proc/self/ns/pid", &pid_namespace) != 0 ||
        member(*parsed, "pidNs").as_integer() != static_cast<std::int64_t>(pid_namespace.st_ino))
    {
        return std::nullopt;
    }
    Peer_target result;
    result.record        = path;
    result.session_id    = std::string(session_id);
    result.pid           = static_cast<int>(pid);
    result.process_start = text(*parsed, "procStart");
    result.socket_path   = text(*parsed, "ipcPath");
    result.token         = text(*parsed, "ipcToken");
    result.launch_token  = std::string(launch_token);
    if (result.process_start.empty() || result.process_start != process_start(result.pid) ||
        result.socket_path.empty() || result.socket_path.size() > 103 ||
        result.socket_path.front() != '/' || result.socket_path.find('\0') != std::string::npos ||
        !hex_string(result.token, 64) || !process_has_binding(result.pid, launch_token))
    {
        return std::nullopt;
    }
    return result;
}

bool wait_ready(int fd, short events, Clock::time_point deadline, const std::function<bool()>& cancelled)
{
    while (Clock::now() < deadline && !cancelled()) {
        pollfd request{fd, events, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int result = poll(&request, 1, static_cast<int>(std::clamp<std::int64_t>(remaining + 1, 0, 50)));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return false;
        }
        if (result > 0) {
            return (request.revents & events) != 0;
        }
    }
    return false;
}

bool same_process(int socket, const Peer_target& target)
{
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
        credentials.pid == target.pid && credentials.uid == getuid();
}

Descriptor connect_peer(const Peer_target& target, Clock::time_point deadline,
    const std::function<bool()>& cancelled)
{
    Descriptor socket_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket_fd.get() < 0 || cancelled()) {
        return Descriptor();
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, target.socket_path.c_str(), target.socket_path.size() + 1);
    if (connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno != EINPROGRESS || !wait_ready(socket_fd.get(), POLLOUT, deadline, cancelled)) {
            return Descriptor();
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(socket_fd.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            return Descriptor();
        }
    }
    if (!same_process(socket_fd.get(), target)) {
        return Descriptor();
    }
    return socket_fd;
}

class Receipt_inbox
{
public:
    Receipt_inbox()
    {
        char pattern[] = "/tmp/logonomic-qwen-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        if (!directory) {
            return;
        }
        m_directory = directory;
        m_path = m_directory + "/receipt.sock";
        std::array<unsigned char, 32> token_bytes{};
        std::size_t offset = 0;
        while (offset < token_bytes.size()) {
            const auto count = getrandom(token_bytes.data() + offset, token_bytes.size() - offset, 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(count);
        }
        constexpr char digits[] = "0123456789abcdef";
        for (const auto byte : token_bytes) {
            m_token.push_back(digits[byte >> 4]);
            m_token.push_back(digits[byte & 15]);
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
        m_ready = m_socket.get() >= 0 &&
            bind(m_socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            chmod(m_path.c_str(), 0600) == 0 && listen(m_socket.get(), 8) == 0;
    }

    ~Receipt_inbox()
    {
        if (!m_path.empty()) {
            unlink(m_path.c_str());
        }
        if (!m_directory.empty()) {
            rmdir(m_directory.c_str());
        }
    }

    bool ready() const { return m_ready; }
    const std::string& path() const { return m_path; }
    const std::string& token() const { return m_token; }

    std::string receive(const Peer_target& target, std::string_view message_id,
        Clock::time_point deadline, const std::function<bool()>& cancelled)
    {
        while (wait_ready(m_socket.get(), POLLIN, deadline, cancelled)) {
            const Descriptor connection(accept4(m_socket.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (connection.get() < 0 || !same_process(connection.get(), target)) {
                continue;
            }
            std::string bytes;
            bool authenticated = false;
            bool valid = true;
            while (valid && bytes.size() <= k_record_limit && wait_ready(connection.get(), POLLIN, deadline, cancelled)) {
                std::array<char, 4096> buffer{};
                const auto count = recv(connection.get(), buffer.data(), buffer.size(), 0);
                if (count <= 0) {
                    break;
                }
                bytes.append(buffer.data(), static_cast<std::size_t>(count));
                auto end = bytes.find('\n');
                while (end != std::string::npos) {
                    const auto frame = parse(std::string_view(bytes).substr(0, end));
                    bytes.erase(0, end + 1);
                    if (!frame || !member(*frame, "msgV").is_integer() ||
                        member(*frame, "msgV").as_integer() != 1)
                    {
                        valid = false;
                        break;
                    }
                    if (!authenticated) {
                        if (text(*frame, "type") != "auth" || text(*frame, "token") != m_token) {
                            valid = false;
                            break;
                        }
                        authenticated = true;
                    }
                    else
                    if (text(*frame, "type") == "control" && text(*frame, "action") == "delivery_status" &&
                        text(*frame, "origMsgId") == message_id && text(*frame, "from") == target.socket_path)
                    {
                        const auto status = text(*frame, "status");
                        if (status == "delivered" || status == "held" || status == "denied" ||
                            status == "refused" || status == "expired" || status == "misaddressed" || status == "dropped")
                        {
                            return status;
                        }
                    }
                    end = bytes.find('\n');
                }
            }
        }
        return "unknown";
    }

private:
    Descriptor  m_socket{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
    std::string m_directory;
    std::string m_path;
    std::string m_token;
    bool        m_ready = false;
};

#endif

} // namespace

std::optional<Peer_target> find_peer(const fs::path& profile, std::string_view session_id, std::string_view launch_token)
{
#ifdef __linux__
    std::optional<Peer_target> result;
    std::error_code error;
    fs::directory_iterator entries(profile / "sessions", error);
    std::size_t count = 0;
    while (!error && entries != fs::directory_iterator()) {
        if (++count > 4096) {
            return std::nullopt;
        }
        const auto candidate = read_peer(entries->path(), session_id, launch_token);
        if (candidate) {
            if (result) {
                return std::nullopt;
            }
            result = candidate;
        }
        entries.increment(error);
    }
    return error ? std::nullopt : result;
#else
    (void)profile;
    (void)session_id;
    (void)launch_token;
    return std::nullopt;
#endif
}

bool peer_reachable(const Peer_target& target)
{
#ifdef __linux__
    return connect_peer(target, Clock::now() + k_exchange_timeout, [] { return false; }).get() >= 0;
#else
    (void)target;
    return false;
#endif
}

Peer_result send_peer(const Peer_target& target, std::string_view message_id,
    std::string_view notification, const std::function<bool()>& cancelled)
{
#ifdef __linux__
    const auto current = read_peer(target.record, target.session_id, target.launch_token);
    if (!current || current->pid != target.pid || current->process_start != target.process_start ||
        current->socket_path != target.socket_path || current->token != target.token)
    {
        return {"misaddressed", false};
    }
    Receipt_inbox inbox;
    if (!inbox.ready()) {
        return {"inbox_unavailable", false};
    }
    const auto deadline = Clock::now() + k_exchange_timeout;
    const auto connection = connect_peer(target, deadline, cancelled);
    if (connection.get() < 0) {
        return {"unreachable", false};
    }
    // Omit fromMode: the host cannot assert the originating agent's approval
    // class. Qwen's own inbound policy must admit or hold this ordinary peer.
    const auto auth = object({{"msgV", Json(1)}, {"type", Json("auth")}, {"token", Json(target.token)}});
    const auto frame = object({
        {"msgV", Json(1)}, {"msgId", Json(message_id)}, {"type", Json("user")},
        {"from", Json(inbox.path())}, {"replyToken", Json(inbox.token())},
        {"fromName", Json("Logonomic mailbox")}, {"toSessionId", Json(target.session_id)},
        {"priority", Json("next")}, {"message", object({{"role", Json("user")}, {"content", Json(notification)}})}});
    const auto bytes = addon::serialize_json(auth) + "\n" + addon::serialize_json(frame) + "\n";
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!wait_ready(connection.get(), POLLOUT, deadline, cancelled)) {
            return {"unknown", offset != 0};
        }
        const auto count = send(connection.get(), bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (count <= 0) {
            return {"unknown", offset != 0};
        }
        offset += static_cast<std::size_t>(count);
    }
    shutdown(connection.get(), SHUT_WR);
    return {inbox.receive(target, message_id, deadline, cancelled), true};
#else
    (void)target;
    (void)message_id;
    (void)notification;
    (void)cancelled;
    return {"platform_unavailable", false};
#endif
}

} // namespace qwen
