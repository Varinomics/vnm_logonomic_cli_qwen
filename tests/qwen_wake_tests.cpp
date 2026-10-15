#include "qwen_json.h"
#include "qwen_peer.h"
#include "qwen_session_wake.h"

#include <logonomic_cli_addon/package_source.h>

#ifdef __linux__
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
#ifdef __linux__
constexpr const char* k_session = "ac285bac-32fa-4764-b789-066ef86d9ab3";
constexpr const char* k_binding = "bfc0b538f38596aef4ac803b9f182cde";
#endif

bool check(bool condition, const char* explanation)
{
    if (!condition) {
        std::cerr << explanation << '\n';
    }
    return condition;
}

#ifdef __linux__

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { m_result = std::move(result); }
    void fail(addon::Result_status, std::string reason, std::string) override { throw std::runtime_error(reason); }
    bool finished() const override { return m_result.has_value(); }

    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
};

std::string native_start()
{
    std::ifstream boot("/proc/sys/kernel/random/boot_id");
    std::ifstream stat("/proc/self/stat");
    std::string boot_id;
    std::string status;
    std::getline(boot, boot_id);
    std::getline(stat, status);
    std::istringstream fields(status.substr(status.rfind(')') + 1));
    std::string ticks;
    for (int field = 3; field <= 22; ++field) {
        fields >> ticks;
    }
    return boot_id + ":" + ticks;
}

class Peer_fixture
{
public:
    Peer_fixture()
    {
        char path[] = "/tmp/logonomic-qwen-test-XXXXXX";
        const auto* created = mkdtemp(path);
        if (!created) {
            throw std::runtime_error("fixture_directory");
        }
        m_root = created;
        fs::create_directory(m_root / "sessions");
        m_socket_path = (m_root / "peer.sock").string();
        m_record_path = m_root / "sessions" / (std::to_string(getpid()) + ".json");
        m_socket = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, m_socket_path.c_str(), m_socket_path.size() + 1);
        if (m_socket < 0 || bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(m_socket, 8)) {
            throw std::runtime_error("fixture_socket");
        }
        struct stat info{};
        if (stat("/proc/self/ns/pid", &info) != 0) {
            throw std::runtime_error("fixture_namespace");
        }
        m_record = qwen::object({
            {"schemaVersion", qwen::Json(1)}, {"pid", qwen::Json(static_cast<int>(getpid()))},
            {"pidNs", qwen::Json(static_cast<std::uint64_t>(info.st_ino))},
            {"procStart", qwen::Json(native_start())}, {"sessionId", qwen::Json(k_session)},
            {"ipcPath", qwen::Json(m_socket_path)}, {"ipcToken", qwen::Json(std::string(64, 'd'))}});
        write_record();
    }

    ~Peer_fixture()
    {
        close(m_socket);
        std::error_code error;
        fs::remove_all(m_root, error);
    }

    const fs::path& profile() const { return m_root; }
    const fs::path& record_path() const { return m_record_path; }
    qwen::Json& record() { return m_record; }
    void write_record() { std::ofstream(m_record_path) << addon::serialize_json(m_record); }

    bool serve(const char* status, bool wrong_auth = false)
    {
        pollfd pending{m_socket, POLLIN, 0};
        if (poll(&pending, 1, 4000) <= 0) {
            return false;
        }
        const int connection = accept(m_socket, nullptr, nullptr);
        std::string bytes;
        std::array<char, 8192> buffer{};
        while (true) {
            const auto count = recv(connection, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                break;
            }
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        close(connection);
        const auto end = bytes.find('\n');
        if (end == std::string::npos) {
            return false;
        }
        const auto auth = qwen::parse(std::string_view(bytes).substr(0, end));
        const auto frame = qwen::parse(std::string_view(bytes).substr(end + 1));
        if (!auth || !frame || qwen::text(*auth, "token") != std::string(64, 'd') ||
            qwen::text(*frame, "toSessionId") != k_session || frame->find("fromMode") ||
            qwen::text(*frame, "fromName") != "Logonomic mailbox" ||
            qwen::text(qwen::member(*frame, "message"), "content") != "/shell @file\nMailbox ready")
        {
            return false;
        }
        const auto reply_path = qwen::text(*frame, "from");
        const auto token = wrong_auth ? "incorrect" : qwen::text(*frame, "replyToken");
        const auto reply_auth = qwen::object({
            {"msgV", qwen::Json(1)}, {"type", qwen::Json("auth")}, {"token", qwen::Json(token)}});
        const auto receipt = qwen::object({
            {"msgV", qwen::Json(1)}, {"type", qwen::Json("control")},
            {"action", qwen::Json("delivery_status")}, {"status", qwen::Json(status)},
            {"origMsgId", qwen::member(*frame, "msgId")}, {"from", qwen::Json(m_socket_path)}});
        const auto reply = addon::serialize_json(reply_auth) + "\n" + addon::serialize_json(receipt) + "\n";
        const int response = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, reply_path.c_str(), reply_path.size() + 1);
        const bool connected = connect(response, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        const auto sent = connected ? send(response, reply.data(), reply.size(), MSG_NOSIGNAL) : -1;
        close(response);
        return sent == static_cast<ssize_t>(reply.size());
    }

private:
    fs::path    m_root;
    fs::path    m_record_path;
    std::string m_socket_path;
    qwen::Json  m_record;
    int         m_socket = -1;
};

bool registry_binding()
{
    Peer_fixture fixture;
    const auto peer = qwen::find_peer(fixture.profile(), k_session, k_binding);
    bool ok = check(peer.has_value(), "native registry binds exact current process and session");
    ok &= check(!qwen::find_peer(fixture.profile(), "another-session", k_binding), "session identity never falls back to newest record");
    ok &= check(!qwen::find_peer(fixture.profile(), k_session, std::string(32, 'a')),
        "another launch of the same native session cannot acquire this launch's binding");
    const auto original = fixture.record();
    for (const auto* field : {"pidNs", "schemaVersion"}) {
        fixture.record().set(field, qwen::Json(9001));
        fixture.write_record();
        ok &= check(!qwen::find_peer(fixture.profile(), k_session, k_binding), "foreign namespace or unknown schema is unavailable");
        fixture.record() = original;
    }
    fixture.record().set("procStart", qwen::Json("another-boot:12931"));
    fixture.write_record();
    ok &= check(!qwen::find_peer(fixture.profile(), k_session, k_binding), "PID reuse cannot bind a stale record");
    fixture.record() = original;
    fixture.record().set("sessionId", qwen::Json("rotated-session"));
    fixture.write_record();
    if (peer) {
        const auto result = qwen::send_peer(*peer, "wake-test", "Mailbox ready", [] { return false; });
        ok &= check(!result.written && result.status == "misaddressed", "native identity is reread before sending");
    }
    fixture.record() = original;
    fixture.write_record();
    fs::copy_file(fixture.record_path(), fixture.record_path().parent_path() /
        (std::to_string(getpid()) + "-aaccee77.json"));
    ok &= check(!qwen::find_peer(fixture.profile(), k_session, k_binding), "duplicate live identities are ambiguous");
    return ok;
}

bool receipt_contract()
{
    // Oracle: Qwen's public cross-session-protocol.md, sections 3–6. These
    // fixtures exchange native frames, without a model or a user's session.
    bool ok = true;
    for (const auto* status : {"delivered", "held", "denied", "refused", "expired", "misaddressed", "dropped"}) {
        Peer_fixture fixture;
        const auto peer = qwen::find_peer(fixture.profile(), k_session, k_binding);
        if (!peer) {
            return false;
        }
        bool served = false;
        std::thread receiver([&] { served = fixture.serve(status); });
        const auto result = qwen::send_peer(*peer, "wake-test", "/shell @file\nMailbox ready", [] { return false; });
        receiver.join();
        ok &= check(served && result.written && result.status == status,
            "exact addressed message preserves attribution and native admission receipt");
    }
    Peer_fixture fixture;
    const auto peer = qwen::find_peer(fixture.profile(), k_session, k_binding);
    if (!peer) {
        return false;
    }
    bool served = false;
    std::thread receiver([&] { served = fixture.serve("delivered", true); });
    const auto result = qwen::send_peer(*peer, "wake-test", "/shell @file\nMailbox ready", [] { return false; });
    receiver.join();
    ok &= check(served && result.written && result.status == "unknown", "unauthenticated receipt never proves acceptance");
    return ok;
}

bool capability_contract()
{
    bool ok = true;
    for (const auto* status : {"delivered", "held", "refused"}) {
        Peer_fixture fixture;
        addon::Session_launch_context launch;
        launch.launch_environment = addon::Launch_environment{
            "linux-x86_64", qwen::Json::object(), addon::Environment_bootstrap_evidence{fixture.profile().string()}};
        const auto preparation = qwen::prepare_session_wake(launch);
        addon::Session_wake_notify_request request;
        request.target.provider_session_id = k_session;
        request.target.working_directory = fixture.profile().string();
        request.target.launcher_argv = {"qwen"};
        request.target.launch_environment = *launch.launch_environment;
        request.target.correlation.transition_nonce = std::string(32, 'e');
        request.target.binding = preparation.binding;
        request.target.binding.set("launch_token", qwen::Json(k_binding));
        request.notification_id = "mailbox-notification";
        request.instruction = "/shell @file\nMailbox ready";
        bool served = false;
        std::thread receiver([&] { served = fixture.serve(status); });
        Context context;
        context.m_request = {"wake", {"session-wake", 1}, "notify", addon::encode_session_wake_notify_request(request)};
        qwen::handle_session_wake(context);
        receiver.join();
        const auto result = context.m_result ? addon::decode_session_wake_notify_result(*context.m_result, nullptr) : std::nullopt;
        const auto expected = std::string_view(status) == "refused"
            ? addon::Session_wake_disposition::UNAVAILABLE
            : addon::Session_wake_disposition::ACCEPTED;
        ok &= check(served && result && result->disposition == expected,
            "native receipt alone decides SDK retention or refusal");
        if (std::string_view(status) == "held") {
            ok &= check(result && result->receipt && result->receipt->starts_with("held:"),
                "retained holds suppress retries without asserting model-input delivery");
        }
        request.target.binding.set("account_key", qwen::Json("different-account"));
        context.m_request = {"inspect", {"session-wake", 1}, "inspect",
            addon::encode_session_wake_inspect_request({request.target})};
        context.m_result.reset();
        qwen::handle_session_wake(context);
        const auto readiness = context.m_result ? addon::decode_session_wake_readiness(*context.m_result, nullptr) : std::nullopt;
        ok &= check(readiness && !readiness->available, "a changed profile binding cannot notify a live peer");
    }
    return ok;
}

#endif

} // namespace

int main(int argc, char** argv)
{
#ifdef __linux__
    if (argc == 4 && std::string_view(argv[1]) == "--native-fixture") {
        const auto target = qwen::find_peer(argv[2], argv[3], k_binding);
        if (!target) {
            return 2;
        }
        const auto result = qwen::send_peer(*target, "logonomic-native-fixture", "/shell @file\nMailbox ready",
            [] { return false; });
        return check(result.written && result.status == "delivered", "upstream peer SDK did not confirm native delivery") ? 0 : 3;
    }
    return registry_binding() && receipt_contract() && capability_contract() ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    const auto preparation = qwen::prepare_session_wake({});
    return check(!preparation.available && preparation.reason == "qwen_peer_platform_unavailable",
        "native peer inbox explicitly reports capability unavailable on this platform") ? 0 : 1;
#endif
}
