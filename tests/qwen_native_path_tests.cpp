#include "qwen_profile.h"
#include "qwen_resume.h"

#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace test = addon::test;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

fs::path io_path(const fs::path& path)
{
    return addon::usage_cache_io_path(path.lexically_normal());
}

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(io_path(path.parent_path()));
    std::ofstream output(io_path(path), std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    require(output.good(), "native source fixture write failed");
}

std::string row(std::size_t index, unsigned tokens)
{
    const auto id = "session-" + std::to_string(index);
    return "{\"sessionId\":\"" + id + "\",\"uuid\":\"" + id +
        "\",\"timestamp\":\"2026-09-12T12:00:00.000Z\",\"cwd\":\"\",\"type\":\"system\","
        "\"subtype\":\"ui_telemetry\",\"systemPayload\":{\"uiEvent\":{"
        "\"event.name\":\"qwen-code.api_response\",\"model\":\"qwen3\","
        "\"input_token_count\":" + std::to_string(tokens) +
        ",\"output_token_count\":0,\"cached_content_token_count\":0,\"thoughts_token_count\":0,"
        "\"total_token_count\":" + std::to_string(tokens) + "}}}\n";
}

void usage_contract(const fs::path& package, const fs::path& fixture, bool long_profile)
{
    auto profile = fixture / "profile";
    if (long_profile) {
        while (profile.native().size() < 280) {
            profile /= "native-profile-directory-component";
        }
    }
    const auto projects = profile / "projects";
    auto project = projects / "repository";
    std::vector<fs::path> sources{project / "chats" / "short.jsonl"};
    if (!long_profile) {
        project = projects / std::string(190 - projects.native().size(), 'p');
        const auto chats = project / "chats";
        for (const auto length : {259, 260}) {
            const auto path = chats / (std::string(length - chats.native().size() - 7, 's') + ".jsonl");
            require(path.native().size() == (std::size_t)length, "exact native filename boundary");
            sources.push_back(path);
        }
        project = projects / std::string(210, 'd');
        sources.push_back(project / "chats" /  "long-directory.jsonl");

    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        write(sources[index], row(index, 11));
    }
    const auto summary = [&](unsigned tokens) {
        write(profile / "usage_record.jsonl",
            "{\"version\":1,\"sessionId\":\"summary-only\",\"timestamp\":1789200000000,"
            "\"project\":\"\",\"models\":{\"qwen3\":{\"requests\":1,\"inputTokens\":" +
            std::to_string(tokens) + ",\"outputTokens\":0,\"cachedTokens\":0,\"totalTokens\":" +
            std::to_string(tokens) + "}}}\n");
    };
    summary(11);
    const auto profile_alias = profile / "unused" / "..";
    const auto native_profile = addon::path_to_utf8(profile_alias);
    const auto account = qwen::account_key(profile_alias);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.qwen", &error, account);
    require(allocated.has_value(), error);
    const auto storage = addon::path_from_utf8(*allocated);

    addon::Usage_scan_request request;
    request.context.launcher = {"qwen", addon::k_runner_direct, {}};
    request.context.launcher_argv = {"qwen"};
    request.context.profile_storage = addon::Profile_storage_context{1, *allocated};
    request.context.account_context = addon::Account_context{2, account, native_profile};
    request.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(),
        addon::Environment_bootstrap_evidence{native_profile}};
    addon::Simulator_invocation_options invocation;
    invocation.profile_storage = request.context.profile_storage;
    const auto acknowledgment = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledgment, 6000);
    require(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "adapter acknowledges native path fixture storage");

    const auto select = [&](const char* name, std::optional<addon::Usage_cache_reference> accepted) {
        const auto candidate = storage / name;
        fs::create_directories(io_path(candidate));
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(candidate)};
    };
    const auto scan = [&](unsigned expected_tokens) {
        const auto page = test::usage_scan_pages(host, request, 3);
        require(page.records.size() == sources.size() + 1 &&
            test::usage_token_total(page) == (sources.size() + 1) * expected_tokens && page.cache_candidate,
            "every short, long and nested transcript contributes its exact usage");
        for (const auto& record : page.records) {
            require(record.source_path && !record.source_path->starts_with("\\\\?\\"),
                "reported native source paths retain ordinary spelling");
        }
        return page;
    };

    request.mode = "repair";
    select("full", std::nullopt);
    const auto first = scan(11);
    request.mode = "incremental";
    select("unchanged", first.cache_candidate);
    const auto unchanged = scan(11);
    for (std::size_t index = 0; index < first.records.size(); ++index) {
        require(first.records[index].record_id == unchanged.records[index].record_id &&
            first.records[index].source_path == unchanged.records[index].source_path,
            "incremental replay preserves retained source and record identities");
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        write(sources[index], row(index, 123));
    }
    summary(123);
    select("changed", unchanged.cache_candidate);
    scan(123);
    require(host.shutdown(1000, nullptr), "native path fixture adapter exits");
    std::cout << "qwen native usage: " << (long_profile ? "long profile" : "259/260 and long directories")
              << " passed\n";
}

bool account_paths(const fs::path& package, const fs::path& root)
{
    addon::Simulator_options options;
    options.package_directory = package;
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), failure.detail);
    const auto invoke = [&](const addon::Capability_pair& capability, std::string operation, addon::Json_value payload) {
        const auto id = host.invoke(capability, std::move(operation), std::move(payload), 5000);
        auto outcome = host.wait_outcome(id, 6000);
        require(outcome.has_value(), "bounded account path operation completes");
        return *outcome;
    };
    addon::Accounts_launcher_request accounts;
    accounts.launcher = {"qwen", "cmd", {}};
    accounts.normalization_directory = addon::path_to_utf8(root);
    const auto discover = [&](const fs::path& path) {
        accounts.launch_environment = {addon::host_platform_id(), addon::Json_value::object(),
            addon::Environment_bootstrap_evidence{addon::path_to_utf8(path)}};
        const auto outcome = invoke({"accounts", 2}, "discover", addon::encode_accounts_launcher_request(accounts));
        const auto records = addon::decode_accounts_result(outcome.payload, &failure);
        require(outcome.status == addon::Host_status::SUCCESS && records && records->size() == 1,
            "the selected native Qwen profile is discovered");
        return records->front();
    };
    const auto prepare = [&](std::string key, const fs::path& retained) {
        addon::Prepare_request launch;
        launch.launcher = accounts.launcher;
        launch.launcher_argv = {"qwen"};
        launch.working_directory = accounts.normalization_directory;
        launch.launch_environment = accounts.launch_environment;
        launch.account_context = addon::Account_context{2, std::move(key), addon::path_to_utf8(retained)};
        return invoke({"launch-preparation", 3}, "prepare", addon::encode_prepare_request(launch));
    };
    fs::create_directory(io_path(root / "available "));
    fs::create_directory(root / "other");
    require(discover(root / "available ").status == addon::Account_status::AVAILABLE &&
        discover(root / "other ").status == addon::Account_status::UNAVAILABLE,
        "profile availability follows the exact native trailing-space spelling");
    const auto parent = root / "profiles";
    fs::create_directory(parent);
    const auto upper = parent / "Work";
    const auto lower = parent / "work";
    const auto historical = discover(upper).adapter_account_key;
    require(historical == discover(lower).adapter_account_key, "ordinary Windows directories preserve the saved key");
    HANDLE directory = CreateFileW(parent.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "open the isolated directory to set native case sensitivity");
    FILE_CASE_SENSITIVE_INFO information{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &information, sizeof(information)) != 0;
    const auto error = GetLastError();
    CloseHandle(directory);
    if (!enabled && (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER ||
            error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)) return false;
    require(enabled && fs::create_directory(upper) && fs::create_directory(lower), "create two native case-distinct profiles");
    const auto upper_key = discover(upper).adapter_account_key;
    const auto lower_key = discover(lower).adapter_account_key;
    require(upper_key != lower_key && upper_key != historical && lower_key != historical,
        "both case-distinct profiles have independent account keys");
    require(prepare(historical, lower).status == addon::Host_status::SUCCESS &&
        prepare(lower_key, lower).status == addon::Host_status::SUCCESS,
        "current and historical matching grants prepare the same profile");
    for (const auto& foreign : {upper_key, lower_key, historical}) {
        require(prepare(foreign, upper).status != addon::Host_status::SUCCESS,
            "a sibling profile cannot supply a current or historical grant");
    }
    const std::string session = "86716ee2-2c26-44c3-b8f6-791d10e9f44e";
    const auto transcript = lower / (session + ".jsonl");
    addon::Session_launch_context launch;
    launch.launch_environment = accounts.launch_environment;
    launch.working_directory = addon::path_to_utf8(lower);
    launch.launcher_argv = {"qwen"};
    const auto inspect = [&](const fs::path& profile, const fs::path& cwd) {
        const auto body = addon::json::object({{"profile_home", qwen::Json(addon::path_to_utf8(profile))},
            {"cwd", qwen::Json(addon::path_to_utf8(cwd))}, {"transcript_path", qwen::Json(addon::path_to_utf8(transcript))}});
        return qwen::inspect_resume(launch, qwen::make_resume_record(session, body)).preparation_failure;
    };
    require(inspect(upper, lower) == "qwen_resume_scope_mismatch" &&
        inspect(lower, upper) == "qwen_resume_scope_mismatch" &&
        inspect(lower, upper / ".qwen" / "worktrees" / "child") == "qwen_resume_scope_mismatch",
        "a saved resume refuses a different case-sensitive profile, project or worktree parent");
    require(fs::remove(upper) && fs::remove(lower) && fs::remove(parent), "remove the original case-sensitive scope");
    require(discover(lower).adapter_account_key == historical &&
        prepare(historical, upper).status != addon::Host_status::SUCCESS,
        "a missing intermediate directory cannot authorize a differently spelled historical locator");
    require(host.shutdown(1000, nullptr), "account path adapter exits");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / ("qwen-native-paths-" + addon::random_nonce_hex());
    int result = 0;
    try {
        require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--account-paths"), "expected built package directory");
        fs::create_directories(root);
        const auto fixture = fs::canonical(root);
        const auto package = addon::path_from_utf8(argv[1]);
        if (argc == 3) {
            if (!account_paths(package, fixture)) result = 77;
        }
        else {
            usage_contract(package, fixture / "files", false);
            usage_contract(package, fixture / "profile", true);
        }
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::error_code error;
    fs::remove_all(io_path(root), error);
    if (error) {
        std::cerr << "native path fixture cleanup: " << error.message() << '\n';
        result = 1;
    }
    return result;
}
