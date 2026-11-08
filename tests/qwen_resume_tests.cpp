#include "qwen_resume.h"

#include "qwen_profile.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Json = addon::Json_value;
using Availability = addon::Resume_availability;
using Content = addon::Resume_content;

constexpr std::string_view k_session = "e4562c53-be62-4f6e-b383-1922774c2627";
constexpr std::string_view k_other_session = "cb1cc368-3943-41a5-b995-51234af67378";

void check(bool condition, std::string_view description)
{
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("qwen-resume-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(addon::usage_cache_io_path(m_path), error);
    }

    fs::path m_path;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(addon::usage_cache_io_path(path.parent_path()));
    std::ofstream output(addon::usage_cache_io_path(path), std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(output.good(), "write native transcript fixture");
}

std::string read(const fs::path& path)
{
    std::ifstream input(addon::usage_cache_io_path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string ascii_project_name(const fs::path& cwd)
{
    auto result = addon::path_to_utf8(cwd);
    for (auto& c : result) {
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
#endif
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            c = '-';
        }
    }
    return result;
}

struct Resume_fixture
{
    fs::path profile;
    fs::path runtime;
    fs::path cwd;
    fs::path source;
    addon::Session_launch_context context;
    Json record;

    explicit Resume_fixture(const fs::path& root)
    :
        profile(root / "profile"),
        runtime(root / "runtime"),
        cwd(root / "project" / "nested")
    {
        fs::create_directories(addon::usage_cache_io_path(profile));
        fs::create_directories(addon::usage_cache_io_path(cwd));
        source = runtime / "projects" / ascii_project_name(cwd) / "chats" /
            (std::string(k_session) + ".jsonl");
        context.launcher = {"qwen", addon::k_runner_direct, {}};
        context.launcher_argv = {"qwen"};
        context.working_directory = addon::path_to_utf8(cwd);
        context.account_context = addon::Account_context{2, qwen::account_key(profile), addon::path_to_utf8(profile)};
        context.launch_environment = addon::Launch_environment{addon::host_platform_id(),
            json::object({{"QWEN_HOME", Json(addon::path_to_utf8(profile))},
                {"QWEN_RUNTIME_DIR", Json(addon::path_to_utf8(runtime))}}),
            addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)}};
        record = qwen::make_resume_record(k_session, json::object({
            {"transcript_path", Json(addon::path_to_utf8(source))},
            {"cwd", Json(addon::path_to_utf8(cwd))}, {"profile_home", Json(addon::path_to_utf8(profile))}}));
        check(qwen::valid_resume_record(record) && record.find("native_source"), "capture complete native provenance");
    }

    Json row(std::string uuid, std::optional<std::string> parent, std::string type,
        std::string subtype = {}, std::string_view session = k_session) const
    {
        // Minimal native ChatRecord fields observed through real Qwen 0.27.0
        // SessionService JSONL. Type, not message.role, distinguishes metadata.
        auto result = json::object({{"uuid", Json(std::move(uuid))},
            {"parentUuid", parent ? Json(*parent) : Json()}, {"sessionId", Json(std::string(session))},
            {"timestamp", Json("2026-11-08T09:27:09.152Z")}, {"cwd", Json(addon::path_to_utf8(cwd))},
            {"version", Json("0.27.0")}, {"type", Json(type)},
            {"provenance", Json(type == "user" ? "real_user" : type == "assistant" ? "assistant_output" : "system")}});
        if (!subtype.empty()) {
            result.set("subtype", Json(std::move(subtype)));
        }
        if (type != "system") {
            result.set("message", json::object({{"role", Json(type == "assistant" ? "model" : "user")},
                {"parts", Json(Json::Array{json::object({{"text", Json("Retained native turn")}})})}}));
        }
        return result;
    }
};

std::string rows(std::initializer_list<Json> records)
{
    std::string result;
    for (const auto& record : records) {
        result += addon::serialize_json(record) + '\n';
    }
    return result;
}

void expect(const qwen::Resume_inspection& actual, Availability availability, Content content,
    bool preparation_allowed, std::string_view description)
{
    check(actual.availability == availability && actual.content == content &&
        actual.preparation_allowed == preparation_allowed, description);
}

void content_contract(const fs::path& root)
{
    Resume_fixture fixture(root);
    const auto inspect = [&] { return qwen::inspect_resume(fixture.context, fixture.record); };
    expect(inspect(), Availability::UNKNOWN, Content::UNKNOWN, true,
        "fresh /clear and moved transcripts are not inferred empty from a missing file");

    auto title = fixture.row("title", {}, "system", "custom_title");
    title.set("systemPayload", json::object({{"customTitle", Json("A named but empty conversation")}}));
    auto memory = fixture.row("memory", "title", "system", "memory_catalog_snapshot");
    memory.set("message", json::object({{"role", Json("user")},
        {"parts", Json(Json::Array{json::object({{"text", Json("Native startup context")}})})}}));
    auto user = fixture.row("user", "memory", "user");
    auto assistant = fixture.row("assistant", "user", "assistant");

    write(fixture.source, rows({title, memory}));
    expect(inspect(), Availability::AVAILABLE, Content::EMPTY, true,
        "complete native title/startup metadata is resumable but has no conversation content");
    write(fixture.source, rows({title, memory, user, assistant}));
    const auto native_bytes = read(fixture.source);
    expect(inspect(), Availability::AVAILABLE, Content::NONEMPTY, true,
        "persisted native turns remain nonempty without live prompt or assistant hooks");
    check(read(fixture.source) == native_bytes, "inspection leaves the native transcript unchanged");

    auto fork = fixture.row("copied-user", {}, "user");
    fork.set("forkedFrom", json::object({{"sessionId", Json(std::string(k_other_session))},
        {"messageUuid", Json("original-message")}}));
    write(fixture.source, rows({fork, fixture.row("branch-title", "copied-user", "system", "custom_title")}));
    expect(inspect(), Availability::AVAILABLE, Content::NONEMPTY, true,
        "a native copied branch is nonempty before a new local prompt");

    write(fixture.source, rows({user, fixture.row("new-root", {}, "system", "custom_title")}));
    expect(inspect(), Availability::AVAILABLE, Content::EMPTY, true,
        "discarded branches do not reappear in the retained active chain");

    auto fragment = assistant;
    fragment.set("message", json::object({{"role", Json("model")},
        {"parts", Json(Json::Array{json::object({{"text", Json("second fragment")}})})}}));
    write(fixture.source, rows({title, memory, user, assistant, fragment}));
    expect(inspect(), Availability::AVAILABLE, Content::NONEMPTY, true,
        "native duplicate UUID fragments preserve the same logical message");

    const std::vector<std::pair<std::string, std::string>> unknown_cases{
        {"unterminated record", rows({title, memory}) + R"({"type":"user")"},
        {"malformed JSONL", rows({title}) + "not JSON\n"},
        {"missing parent", rows({fixture.row("orphan", "missing", "user")})},
        {"parent cycle", rows({fixture.row("a", "b", "user"), fixture.row("b", "a", "assistant")})},
        {"conflicting fragments", rows({fixture.row("a", {}, "user"), fixture.row("a", "missing", "user")})},
        {"inherited bootstrap", rows({fixture.row("bootstrap", {}, "system", "agent_bootstrap")})},
        {"compressed snapshot", rows({fixture.row("compressed", {}, "system", "chat_compression")})},
        {"unknown system record", rows({fixture.row("future", {}, "system", "future_snapshot")})},
        {"unknown message type", rows({fixture.row("future", {}, "future")})},
        {"unsupported managed format", rows({fixture.row("managed", {}, "system", "managed_session_header_v1")})},
    };
    for (const auto& [description, bytes] : unknown_cases) {
        write(fixture.source, bytes);
        expect(inspect(), Availability::UNKNOWN, Content::UNKNOWN, true, description);
    }
    auto unknown_part = fixture.row("user", {}, "user");
    unknown_part.set("message", json::object({{"role", Json("user")},
        {"parts", Json(Json::Array{json::object({{"futureContent", Json("retained")}})})}}));
    write(fixture.source, rows({unknown_part}));
    expect(inspect(), Availability::UNKNOWN, Content::UNKNOWN, true, "unknown content parts are not empty");
    write(fixture.source, rows({fixture.row("other", {}, "user", {}, k_other_session)}));
    expect(inspect(), Availability::UNKNOWN, Content::UNKNOWN, false,
        "mismatched native identity cannot retarget explicit preparation");

    write(fixture.source, rows({fixture.row("artifact", {}, "system", "session_artifact_snapshot")}));
    expect(inspect(), Availability::ABSENT, Content::EMPTY, false,
        "native loadSession rejects a complete side-metadata log with no conversation chain");
    write(fixture.source, "");
    expect(inspect(), Availability::ABSENT, Content::EMPTY, false,
        "a fully read empty JSONL has no resumable native records");
    fs::remove(addon::usage_cache_io_path(fixture.source));
    fs::create_directory(addon::usage_cache_io_path(fixture.source));
    expect(inspect(), Availability::UNKNOWN, Content::UNKNOWN, true, "unreadable native source stays unknown");
    fs::remove(addon::usage_cache_io_path(fixture.source));
    write(fixture.source, rows({fixture.row("user", {}, "user")}));
    expect(qwen::inspect_resume(fixture.context, fixture.record, [] { return true; }),
        Availability::UNKNOWN, Content::UNKNOWN, false, "cancelled inspection does not admit preparation");
}

void scope_contract(const fs::path& root)
{
    Resume_fixture original(root / "original");
    Resume_fixture other(root / "other");
    write(original.source, rows({original.row("user", {}, "user")}));
    write(other.source, rows({other.row("title", {}, "system", "custom_title")}));
    expect(qwen::inspect_resume(other.context, original.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "same UUID in another profile cannot satisfy captured native provenance");

    auto switched = original.context;
    switched.launch_environment->effective_environment.set("QWEN_RUNTIME_DIR", Json(addon::path_to_utf8(other.runtime)));
    const auto other_store_source = other.runtime / "projects" / ascii_project_name(original.cwd) / "chats" /
        original.source.filename();
    write(other_store_source, rows({original.row("title", {}, "system", "custom_title")}));
    expect(qwen::inspect_resume(switched, original.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "changing runtime root cannot retarget a captured same-ID conversation");
    const auto legacy = qwen::make_resume_record(k_session, Json::object());
    expect(qwen::inspect_resume(switched, legacy), Availability::AVAILABLE, Content::EMPTY, true,
        "legacy records follow native exact project/runtime selection without scanning other projects");
    auto relative_runtime = original.context;
    relative_runtime.launch_environment->effective_environment.set("QWEN_RUNTIME_DIR", Json("../../runtime"));
    expect(qwen::inspect_resume(relative_runtime, original.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "relative native runtime paths resolve against the requested launch directory");
    switched.working_directory = addon::path_to_utf8(other.cwd);
    expect(qwen::inspect_resume(switched, original.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "changing project cannot retarget a captured native conversation");

    auto mismatched_account = original.context;
    mismatched_account.account_context = other.context.account_context;
    expect(qwen::inspect_resume(mismatched_account, original.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "a profile account mismatch never becomes filesystem absence");
    auto no_bootstrap = original.context;
    no_bootstrap.launch_environment->bootstrap.reset();
    expect(qwen::inspect_resume(no_bootstrap, legacy), Availability::UNKNOWN, Content::UNKNOWN, false,
        "inspection never substitutes its own ambient profile for missing bootstrap evidence");

    auto scoped = original.context;
    scoped.launcher_argv = {"qwen", "--cwd", addon::path_to_utf8(other.cwd)};
    expect(qwen::inspect_resume(scoped, original.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "uninterpreted launcher scope cannot admit a captured conversation");
    expect(qwen::inspect_resume(scoped, legacy), Availability::UNKNOWN, Content::UNKNOWN, true,
        "legacy @2 history is not refused solely because provenance predates inspection");

    auto no_file = legacy;
    no_file.set("provider_session_id", Json(std::string(k_other_session)));
    expect(qwen::inspect_resume(original.context, no_file), Availability::UNKNOWN, Content::UNKNOWN, true,
        "missing legacy history stays available for native explicit resume attempts");

    auto worktree = original.row("worktree-user", {}, "user");
    worktree.set("cwd", Json(addon::path_to_utf8(original.cwd / ".qwen" / "worktrees" / "branch")));
    write(original.source, rows({worktree}));
    expect(qwen::inspect_resume(original.context, original.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "native worktree cwd is admitted by its innermost owning project");
    auto foreign = original.row("foreign", {}, "user");
    foreign.set("cwd", Json(addon::path_to_utf8(other.cwd)));
    write(original.source, rows({foreign}));
    expect(qwen::inspect_resume(original.context, original.record), Availability::UNKNOWN, Content::UNKNOWN, true,
        "unexplained native cwd may need a runtime sidecar and is not declared absent");

#ifndef _WIN32
    write(original.source, rows({original.row("user", {}, "user")}));
    const auto alias = root / "runtime-alias";
    fs::create_directory_symlink(original.runtime, alias);
    auto aliased = original.context;
    aliased.launch_environment->effective_environment.set("QWEN_RUNTIME_DIR", Json(addon::path_to_utf8(alias)));
    expect(qwen::inspect_resume(aliased, original.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "native filesystem aliases resolve the exact same stored transcript");

    auto unicode = original.context;
    const auto unicode_cwd = original.cwd / addon::path_from_utf8("\u00e9\U0001f30d");
    fs::create_directories(unicode_cwd);
    unicode.working_directory = addon::path_to_utf8(unicode_cwd);
    const auto unicode_source = original.runtime / "projects" / (ascii_project_name(original.cwd) + "----") /
        "chats" / original.source.filename();
    auto unicode_row = original.row("unicode-user", {}, "user");
    unicode_row.set("cwd", Json(unicode.working_directory));
    write(unicode_source, rows({unicode_row}));
    const auto unicode_record = qwen::make_resume_record(k_session, json::object({
        {"transcript_path", Json(addon::path_to_utf8(unicode_source))}, {"cwd", Json(unicode.working_directory)},
        {"profile_home", Json(addon::path_to_utf8(original.profile))}}));
    expect(qwen::inspect_resume(unicode, unicode_record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "Qwen sanitizeCwd replaces UTF-16 units rather than UTF-8 bytes");
#endif
}

void settings_contract(const fs::path& root)
{
    Resume_fixture fixture(root);
    write(fixture.source, rows({fixture.row("user", {}, "user")}));
    auto context = fixture.context;
    context.launch_environment->effective_environment = json::object({
        {"QWEN_HOME", Json(addon::path_to_utf8(fixture.profile))},
        {"QWEN_CODE_SYSTEM_SETTINGS_PATH", Json(addon::path_to_utf8(root / "system.json"))},
        {"QWEN_CODE_SYSTEM_DEFAULTS_PATH", Json(addon::path_to_utf8(root / "defaults.json"))}});
    write(fixture.profile / "settings.json", "{\n// Native JSONC\n\"advanced\":{\"runtimeOutputDir\":" +
        addon::serialize_json(Json(addon::path_to_utf8(fixture.runtime))) + ",},\n}\n");
    const auto settings_bytes = read(fixture.profile / "settings.json");
    expect(qwen::inspect_resume(context, fixture.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "native user runtimeOutputDir locates the captured transcript");
    check(read(fixture.profile / "settings.json") == settings_bytes, "inspection never migrates or rewrites settings");

    write(fixture.cwd / ".qwen" / "settings.json", R"({"advanced":{"runtimeOutputDir":"another-runtime"}})");
    expect(qwen::inspect_resume(context, fixture.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "unknown native workspace trust cannot redirect a captured runtime");
    expect(qwen::inspect_resume(context, qwen::make_resume_record(k_session, Json::object())),
        Availability::UNKNOWN, Content::UNKNOWN, true, "legacy preparation survives an uninspected runtime setting");
    write(root / "system.json", addon::serialize_json(json::object({{"advanced", json::object({
        {"runtimeOutputDir", Json(addon::path_to_utf8(fixture.runtime))}})}})));
    expect(qwen::inspect_resume(context, fixture.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "native system runtime override wins over a workspace runtime setting");

    write(fixture.profile / "settings.json", "broken settings");
    expect(qwen::inspect_resume(context, fixture.record), Availability::UNKNOWN, Content::UNKNOWN, false,
        "inaccessible or malformed configuration is not evidence of a default runtime");
    expect(qwen::inspect_resume(fixture.context, fixture.record), Availability::AVAILABLE, Content::NONEMPTY, true,
        "explicit QWEN_RUNTIME_DIR has native precedence over settings runtime selection");

    write(fixture.profile / "settings.json", "{}");
    write(fixture.cwd / ".qwen" / "settings.json", "{}");
    write(root / "system.json", "{}");
    const auto default_source = fixture.profile / "projects" / ascii_project_name(fixture.cwd) / "chats" /
        fixture.source.filename();
    write(default_source, rows({fixture.row("default-user", {}, "user")}));
    expect(qwen::inspect_resume(context, qwen::make_resume_record(k_session, Json::object())),
        Availability::AVAILABLE, Content::NONEMPTY, true,
        "legacy records in the ordinary profile runtime remain inspectable");
}

void record_contract(const fs::path& root)
{
    Resume_fixture fixture(root);
    auto malformed = fixture.record;
    malformed.set("unknown", Json(true));
    check(!qwen::valid_resume_record(malformed), "unknown stored record fields are refused");
    const auto malformed_source = qwen::make_resume_record(k_session, json::object({
        {"transcript_path", Json("relative.jsonl")}, {"cwd", Json(addon::path_to_utf8(fixture.cwd))},
        {"profile_home", Json(addon::path_to_utf8(fixture.profile))}}));
    check(qwen::valid_resume_record(malformed_source) && !malformed_source.find("native_source"),
        "incomplete hook provenance preserves the installed ID-only resume record");
    auto too_long = json::object({{"transcript_path", Json(addon::path_to_utf8(fixture.source))},
        {"cwd", Json(addon::path_to_utf8(fixture.cwd))},
        {"profile_home", Json(addon::path_to_utf8(fixture.profile) + std::string(17000, 'x'))}});
    const auto bounded = qwen::make_resume_record(k_session, too_long);
    check(qwen::valid_resume_record(bounded) && !bounded.find("native_source") &&
        addon::serialize_json_canonical(bounded).size() <= addon::limits::k_max_resume_record_bytes,
        "oversized provenance cannot discard an otherwise valid native SessionStart");
    const auto invalid_id = json::object({{"provider_session_id", Json("../outside")}});
    expect(qwen::inspect_resume(fixture.context, invalid_id), Availability::UNKNOWN, Content::UNKNOWN, false,
        "a resume selector stays one exact native UUID component");
}

void packaged_contract(const fs::path& package, const fs::path& root, unsigned version)
{
    Resume_fixture fixture(root / "native");
    write(fixture.source, rows({fixture.row("user", {}, "user")}));
    addon::Simulator_options options;
    options.package_directory = package;
    options.scratch_directory = root / "host";
    if (version == 3) {
        options.adopted_capabilities = std::vector<addon::Capability_pair>{{"interactive-resume", 2},
            {"interactive-resume", 3}};
    }
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    addon::Resume_inspect_request request{fixture.context, fixture.record};
    const auto invoke = [&] {
        const auto id = host.invoke({"interactive-resume", static_cast<std::uint16_t>(version)},
            addon::k_inspect_resume_operation, addon::encode_resume_inspect_request(request), 10000);
        const auto outcome = host.wait_outcome(id, 12000);
        check(outcome && outcome->status == addon::Host_status::SUCCESS,
            outcome ? outcome->message : "packaged resume inspection timed out");
        const auto result = addon::decode_resume_inspect_result(outcome->payload, &failure,
            static_cast<std::uint16_t>(version));
        check(result && addon::validate_resume_inspection(request, *result, &failure),
            "packaged inspection acknowledges its complete exact resolved request");
        check((outcome->payload.find("content") != nullptr) == (version == 4),
            "only interactive-resume@4 emits content evidence");
        return *result;
    };
    const auto first = invoke();
    check(first.availability == Availability::AVAILABLE &&
        first.content == (version == 4 ? Content::NONEMPTY : Content::UNKNOWN),
        "installed old and new hosts inspect the same native conversation");
    auto changed = request;
    changed.context.working_directory = addon::path_to_utf8(root);
    check(!addon::validate_resume_inspection(changed, first, &failure),
        "an inspection digest cannot be reused for a changed project");
    write(fixture.source, rows({fixture.row("title", {}, "system", "custom_title")}));
    check(invoke().content == (version == 4 ? Content::EMPTY : Content::UNKNOWN),
        "a fresh packaged inspection reevaluates native content");
    fs::remove(addon::usage_cache_io_path(fixture.source));
    const auto missing = invoke();
    check(missing.availability == Availability::UNKNOWN && missing.content == Content::UNKNOWN,
        "deleted or relocated native history stays unknown across the process boundary");
    request.resume_record = qwen::make_resume_record(k_session, Json::object());
    check(invoke().availability == Availability::UNKNOWN, "packaged inspector continues to accept legacy stored records");
    int exit_code = -1;
    check(host.shutdown(1000, &exit_code) && exit_code == 0, "packaged inspector shuts down cleanly");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "expected staged package directory");
        Workspace workspace;
        content_contract(workspace.m_path / "content");
        scope_contract(workspace.m_path / "scope");
        settings_contract(workspace.m_path / "settings");
        record_contract(workspace.m_path / "records");
        packaged_contract(addon::path_from_utf8(argv[1]), workspace.m_path / "packaged-v3", 3);
        packaged_contract(addon::path_from_utf8(argv[1]), workspace.m_path / "packaged-v4", 4);
#ifdef _WIN32
        // Qwen repeats its sanitized cwd inside the transcript path, so valid
        // ordinary workspaces can exceed MAX_PATH without a long component.
        const auto long_root = workspace.m_path / "native-long-path" / std::string(80, 'p');
        Resume_fixture long_fixture(long_root / "inspection");
        check(long_fixture.source.native().size() > 260, "exercise a native transcript beyond MAX_PATH");
        write(long_fixture.source, rows({long_fixture.row("user", {}, "user")}));
        expect(qwen::inspect_resume(long_fixture.context, long_fixture.record),
            Availability::AVAILABLE, Content::NONEMPTY, true,
            "long native paths retain exact ordinary resume scope and content");
        packaged_contract(addon::path_from_utf8(argv[1]), long_root / "packaged", 4);
#endif
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
