#include "qwen_resume.h"

#include "qwen_config.h"
#include "qwen_conversation.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Availability = addon::Resume_availability;
using Content = addon::Resume_content;

constexpr std::uintmax_t k_max_transcript_bytes = 64 * 1024 * 1024;
constexpr std::uintmax_t k_max_settings_bytes = 1024 * 1024;

bool session_id_valid(std::string_view value)
{
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        }
        else
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool absolute_path(const Json& value)
{
    return json::bounded_string(value, 32768) && addon::path_from_utf8(value.as_string()).is_absolute();
}

bool native_source_valid(const Json& source, std::string_view session_id)
{
    return json::exact_keys(source, {"transcript_path", "cwd", "profile_home"}) &&
        absolute_path(json::member(source, "transcript_path")) &&
        absolute_path(json::member(source, "cwd")) &&
        absolute_path(json::member(source, "profile_home")) &&
        path_string(addon::path_from_utf8(json::text(source, "transcript_path")).filename()) ==
            std::string(session_id) + ".jsonl";
}

bool same_path(const fs::path& left, const fs::path& right)
{
    if (left.lexically_normal() == right.lexically_normal()) {
        return true;
    }
    std::error_code error;
    return fs::equivalent(native_io_path(left), native_io_path(right), error) && !error;
}

bool project_text_matches(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return addon::same_native_account_profile(addon::path_from_utf8(left), addon::path_from_utf8(right));
#else
    return left == right;
#endif
}

bool project_matches(std::string_view recorded, std::string_view current)
{
    if (project_text_matches(recorded, current)) {
        return true;
    }
#ifdef _WIN32
    constexpr std::string_view k_worktree_marker = "\\.qwen\\worktrees\\";
#else
    constexpr std::string_view k_worktree_marker = "/.qwen/worktrees/";
#endif
    const auto marker = recorded.rfind(k_worktree_marker);
    return marker != std::string_view::npos && marker > 0 &&
        project_text_matches(recorded.substr(0, marker), current);
}

std::optional<std::string> project_directory_name(std::string_view cwd)
{
    std::string result;
    for (std::size_t i = 0; i < cwd.size();) {
        const auto first = static_cast<unsigned char>(cwd[i++]);
        unsigned codepoint = first;
        std::size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint &= 0x1f;
            remaining = 1;
        }
        else
        if (first >= 0xe0 && first <= 0xef) {
            codepoint &= 0x0f;
            remaining = 2;
        }
        else
        if (first >= 0xf0 && first <= 0xf4) {
            codepoint &= 0x07;
            remaining = 3;
        }
        else
        if (first >= 0x80) {
            return {};
        }
        for (std::size_t n = 0; n < remaining; ++n) {
            if (i == cwd.size() || (static_cast<unsigned char>(cwd[i]) & 0xc0) != 0x80) {
                return {};
            }
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(cwd[i++]) & 0x3f);
        }
        if ((remaining == 1 && codepoint < 0x80) || (remaining == 2 && codepoint < 0x800) ||
            (remaining == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
        {
            return {};
        }
#ifdef _WIN32
        if (codepoint >= 'A' && codepoint <= 'Z') {
            codepoint += 'a' - 'A';
        }
        // These Unicode lowercase mappings produce ASCII before sanitizeCwd's
        // non-Unicode JavaScript regexp replaces the remaining UTF-16 units.
        if (codepoint == 0x212a) {
            codepoint = 'k';
        }
        else
        if (codepoint == 0x130) {
            result += "i-";
            continue;
        }
#endif
        if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
            (codepoint >= '0' && codepoint <= '9'))
        {
            result += static_cast<char>(codepoint);
        }
        else {
            result.append(codepoint > 0xffff ? 2 : 1, '-');
        }
    }
    return result;
}

std::optional<fs::path> resolved_path(std::string_view text, const fs::path& cwd,
    const Environment& environment)
{
    if (text.empty() || text.find('\0') != std::string_view::npos || text.find("${") != std::string_view::npos) {
        return {};
    }
    auto value = addon::path_from_utf8(text);
    if (text == "~" || text.starts_with("~/") || text.starts_with("~\\")) {
#ifdef _WIN32
        const auto home = environment_value(environment, "USERPROFILE");
#else
        const auto home = environment_value(environment, "HOME");
#endif
        if (home.empty() || !addon::path_from_utf8(home).is_absolute()) {
            return {};
        }
        std::string suffix(text.size() > 1 ? text.substr(2) : std::string_view{});
        std::replace(suffix.begin(), suffix.end(), '\\', '/');
        value = addon::path_from_utf8(home) / addon::path_from_utf8(suffix);
    }
#ifdef _WIN32
    if (!value.is_absolute() && value.has_root_name()) {
        return {};
    }
#endif
    if (!value.is_absolute()) {
        value = cwd / value;
    }
    return value.lexically_normal().make_preferred();
}

struct Runtime_setting
{
    bool known = false;
    std::optional<std::string> value;
};

Runtime_setting runtime_setting(const fs::path& path)
{
    const auto io_path = native_io_path(path);
    std::error_code error;
    const auto status = fs::status(io_path, error);
    if (status.type() == fs::file_type::not_found) {
        return {true, {}};
    }
    if (error || !fs::is_regular_file(status)) {
        return {};
    }
    const auto size = fs::file_size(io_path, error);
    if (error || size > k_max_settings_bytes) {
        return {};
    }
    const auto modified = fs::last_write_time(io_path, error);
    if (error) {
        return {};
    }
    std::ifstream input(io_path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
        fs::file_size(io_path, error) != size || error || fs::last_write_time(io_path, error) != modified || error)
    {
        return {};
    }
    const auto settings = parse_settings(bytes);
    const auto* advanced = settings.find("advanced");
    // Older migrations and unknown shapes are not evidence of the default.
    if (settings.find("runtimeOutputDir") || (advanced && !advanced->is_object())) {
        return {};
    }
    const auto* runtime = advanced ? advanced->find("runtimeOutputDir") : nullptr;
    if (!runtime) {
        return {true, {}};
    }
    return runtime->is_string() ? Runtime_setting{true, runtime->as_string()} : Runtime_setting{};
}

std::optional<fs::path> runtime_directory(const resolved_profile_t& profile, const fs::path& cwd)
{
    const auto explicit_runtime = environment_value(profile.environment, "QWEN_RUNTIME_DIR");
    if (!explicit_runtime.empty()) {
        return resolved_path(explicit_runtime, cwd, profile.environment);
    }
    auto system_text = environment_value(profile.environment, "QWEN_CODE_SYSTEM_SETTINGS_PATH");
    if (system_text.empty()) {
#ifdef _WIN32
        system_text = "C:\\ProgramData\\qwen-code\\settings.json";
#elif defined(__APPLE__)
        system_text = "/Library/Application Support/QwenCode/settings.json";
#else
        system_text = "/etc/qwen-code/settings.json";
#endif
    }
    const auto system_path = resolved_path(system_text, cwd, profile.environment);
    if (!system_path) {
        return {};
    }
    const auto defaults_text = environment_value(profile.environment, "QWEN_CODE_SYSTEM_DEFAULTS_PATH");
    const auto defaults_path = defaults_text.empty()
        ? std::optional<fs::path>(system_path->parent_path() / "system-defaults.json")
        : resolved_path(defaults_text, cwd, profile.environment);
    if (!defaults_path) {
        return {};
    }
    const auto system = runtime_setting(*system_path);
    const auto defaults = runtime_setting(*defaults_path);
    const auto user = runtime_setting(profile.home / "settings.json");
    const auto workspace = runtime_setting(cwd / ".qwen" / "settings.json");
    if (!system.known || !defaults.known || !user.known || !workspace.known) {
        return {};
    }
    const auto selected = system.value ? system.value : user.value ? user.value : defaults.value;
    // Workspace trust is native state outside the inspection request. A
    // potentially different workspace override cannot establish this scope.
    if (!system.value && workspace.value && workspace.value != selected) {
        return {};
    }
    if (!selected || selected->empty()) {
        return profile.home;
    }
    return resolved_path(*selected, cwd, profile.environment);
}

bool understood_record(const Json& row)
{
    if (!row.is_object() || json::text(row, "uuid").empty() ||
        !row.find("parentUuid") ||
        (!json::member(row, "parentUuid").is_null() && !json::member(row, "parentUuid").is_string()) ||
        row.find("isSidechain") || (row.find("subtype") && !json::member(row, "subtype").is_string()))
    {
        return false;
    }
    const auto type = json::text(row, "type");
    const auto subtype = json::text(row, "subtype");
    if (type == "system") {
        // These native metadata records do not carry conversation messages.
        // Inherited/compressed snapshots and future subtypes remain unknown.
        constexpr std::string_view k_metadata[] = {
            "custom_title", "ui_telemetry", "memory_catalog_snapshot", "attribution_snapshot",
            "file_history_snapshot", "session_source", "session_model", "session_approval_mode",
            "session_workflow_mode", "session_execution_engine", "branch_checkpoint", "rewind",
            "session_artifact_event", "session_artifact_snapshot", "session_sources_snapshot"
        };
        return std::find(std::begin(k_metadata), std::end(k_metadata), subtype) != std::end(k_metadata);
    }
    if ((type != "user" && type != "assistant" && type != "tool_result") || !subtype.empty()) {
        return false;
    }
    const auto role = json::text(json::member(row, "message"), "role");
    return role == (type == "assistant" ? "model" : "user");
}

Resume_inspection inspect_source(const fs::path& source, std::string_view session_id,
    std::string_view project, Resume_inspection result, const std::function<bool()>& cancelled)
{
    const auto io_source = native_io_path(source);
    std::error_code error;
    const auto status = fs::status(io_source, error);
    // /clear has no transcript until Qwen records a turn, and /cd can move an
    // existing transcript without SessionStart. Missing provenance is unknown.
    if (error || !fs::is_regular_file(status)) {
        return result;
    }
    const auto size = fs::file_size(io_source, error);
    if (error || size > k_max_transcript_bytes) {
        return result;
    }
    const auto modified = fs::last_write_time(io_source, error);
    if (error) {
        return result;
    }
    addon::Contract_failure failure;
    const auto retained = read_conversation_records(source, session_id, cancelled, &failure);
    if (failure.reason_code == "conversation_identity_mismatch") {
        result.preparation_allowed = false;
        result.preparation_failure = "qwen_resume_source_mismatch";
        return result;
    }
    if (!retained || retained->snapshot.incomplete_tail_omitted ||
        fs::file_size(io_source, error) != size || error ||
        fs::last_write_time(io_source, error) != modified || error ||
        (cancelled && cancelled()))
    {
        return result;
    }
    if (!retained->records.empty() &&
        !project_matches(json::text(retained->records.front(), "cwd"), project))
    {
        // Native also permits a runtime sidecar to establish project identity;
        // an unexplained cwd is not conclusive absence.
        return result;
    }
    if (!std::all_of(retained->records.begin(), retained->records.end(), understood_record)) {
        return result;
    }
    bool nonempty = false;
    for (const auto& id : retained->chain) {
        addon::Conversation_message message;
        for (const auto index : retained->fragments.at(id)) {
            if (!append_conversation_record(retained->records[index], message, &failure)) {
                return result;
            }
        }
        nonempty = nonempty || !message.content.empty();
    }
    result.content = nonempty ? Content::NONEMPTY : Content::EMPTY;
    result.availability = retained->chain.empty() ? Availability::ABSENT : Availability::AVAILABLE;
    if (result.availability == Availability::ABSENT) {
        result.preparation_allowed = false;
        result.preparation_failure = "qwen_resume_unavailable";
    }
    return result;
}

} // namespace

bool valid_resume_record(const Json& record)
{
    const auto session = json::text(record, "provider_session_id");
    if (!session_id_valid(session) ||
        addon::serialize_json_canonical(record).size() > addon::limits::k_max_resume_record_bytes)
    {
        return false;
    }
    return json::exact_keys(record, {"provider_session_id"}) ||
        (json::exact_keys(record, {"provider_session_id", "native_source"}) &&
            native_source_valid(json::member(record, "native_source"), session));
}

Json make_resume_record(std::string_view session_id, const Json& body)
{
    auto record = json::object({{"provider_session_id", Json(std::string(session_id))}});
    auto source = json::object({{"transcript_path", json::member(body, "transcript_path")},
        {"cwd", json::member(body, "cwd")}, {"profile_home", json::member(body, "profile_home")}});
    if (session_id_valid(session_id) && native_source_valid(source, session_id)) {
        auto enriched = record;
        enriched.set("native_source", std::move(source));
        if (valid_resume_record(enriched)) {
            return enriched;
        }
    }
    return record;
}

Resume_inspection inspect_resume(const addon::Session_launch_context& context, const Json& record,
    const std::function<bool()>& cancelled)
{
    Resume_inspection result;
    result.preparation_failure = "qwen_resume_scope_unavailable";
    if (!valid_resume_record(record)) {
        result.preparation_failure = "qwen_resume_record_invalid";
        return result;
    }
    if (cancelled && cancelled()) {
        return result;
    }
    try {
        const auto profile = profile_for_context(context);
        const auto* provenance = record.find("native_source");
        result.preparation_allowed = !provenance;
        if (result.preparation_allowed) {
            result.preparation_failure.clear();
        }
        if (provenance && !addon::same_native_account_profile(
            addon::path_from_utf8(json::text(*provenance, "profile_home")).lexically_normal(), profile.home.lexically_normal()))
        {
            result.preparation_failure = "qwen_resume_scope_mismatch";
            return result;
        }
        const auto cwd = addon::normalized_resume_directory(context.working_directory);
        if (!cwd || context.launcher_argv.empty() || !plain_session_options(context.launcher_argv)) {
            return result;
        }
        const auto project = path_string(*cwd);
        if (provenance && !project_matches(json::text(*provenance, "cwd"), project)) {
            result.preparation_failure = "qwen_resume_scope_mismatch";
            return result;
        }
        const auto runtime = runtime_directory(profile, *cwd);
        const auto directory = project_directory_name(project);
        if (!runtime || !directory || (cancelled && cancelled())) {
            return result;
        }
        const auto session = json::text(record, "provider_session_id");
        const auto source = *runtime / "projects" / addon::path_from_utf8(*directory) / "chats" /
            addon::path_from_utf8(session + ".jsonl");
        if (provenance && !same_path(source, addon::path_from_utf8(json::text(*provenance, "transcript_path")))) {
            result.preparation_failure = "qwen_resume_scope_mismatch";
            return result;
        }
        result.preparation_allowed = true;
        result.preparation_failure.clear();
        result = inspect_source(source, session, project, result, cancelled);
        if (cancelled && cancelled()) {
            result = {};
            result.preparation_failure = "qwen_resume_inspection_cancelled";
        }
        return result;
    }
    catch (const std::exception&) {
        return result;
    }
}

} // namespace qwen
