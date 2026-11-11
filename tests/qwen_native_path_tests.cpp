#include "qwen_profile.h"

#include <logonomic_cli_addon_test/usage_scan.h>

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>

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

} // namespace

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / ("qwen-native-paths-" + addon::random_nonce_hex());
    int result = 0;
    try {
        require(argc == 2, "expected built package directory");
        fs::create_directories(root);
        const auto fixture = fs::canonical(root);
        const auto package = addon::path_from_utf8(argv[1]);
        usage_contract(package, fixture / "files", false);
        usage_contract(package, fixture / "profile", true);
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
