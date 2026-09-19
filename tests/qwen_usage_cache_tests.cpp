#include "qwen_profile.h"
#include "qwen_usage.h"

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Cancelled_context final : public addon::Invocation_context
{
public:
    explicit Cancelled_context(const addon::Usage_scan_request& input)
    {
        m_request.capability = {"usage-scan", 3};
        m_request.operation = "scan";
        m_request.payload = addon::encode_usage_scan_request(input, 3);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return true; }
    bool wait_for_cancel(int) override { return true; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value) override { throw std::runtime_error("cancelled scan succeeded"); }
    void fail(addon::Result_status, std::string reason, std::string) override
    {
        throw std::runtime_error(reason);
    }
    bool finished() const override { return false; }

private:
    addon::Invoke_request m_request;
};

class Workspace
{
public:
    Workspace()
    :
        m_path(fs::temp_directory_path() / ("qwen-cache-contract-" + addon::random_nonce_hex()))
    {
        fs::create_directories(m_path);
        m_path = fs::canonical(m_path);
    }

    ~Workspace()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    fs::path m_path;
};

void write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    check(output.good(), "local telemetry fixture write failed");
}

std::vector<fs::path> write_sources(const fs::path& profile, unsigned tokens)
{

    std::string history;
    for (unsigned index = 0; index < 270; ++index) {
        history += "{\"version\":1,\"sessionId\":\"session-" + std::to_string(index) +
            "\",\"timestamp\":1789200000000,\"project\":\"\",\"models\":{\"qwen3\":{\"requests\":1,"
            "\"inputTokens\":" + std::to_string(tokens) + ",\"outputTokens\":0,\"cachedTokens\":0,"
            "\"totalTokens\":" + std::to_string(tokens) + "}}}\n";
    }
    const auto path = profile / "usage_record.jsonl";
    write(path, history);
    return {path};
}

void adapter_cache_contract(const fs::path& package, const fs::path& fixture)
{
    const auto profile = fixture / "native-profile";
    auto sources = write_sources(profile, 10);
    const auto native_profile = addon::path_to_utf8(profile);
    const auto account = qwen::account_key(profile);

    addon::Simulator_options options;
    options.package_directory = package;
    options.profile_storage_root = fixture / "host-storage";
    addon::Host_simulator host(options, nullptr);
    addon::Contract_failure failure;
    check(host.start(&failure), failure.detail);
    std::string error;
    const auto allocated = addon::allocate_profile_storage_root(
        host.profile_storage_root(), "logonomic.cli.qwen", &error, account);
    check(allocated.has_value(), error);
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
    const auto acknowledge = host.invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto acknowledged = host.wait_outcome(acknowledge, 6000);
    check(acknowledged && acknowledged->status == addon::Host_status::SUCCESS,
        "packaged adapter acknowledges allocated storage");

    const auto outcome_for = [&](const addon::Usage_scan_request& input, std::uint16_t version) {
        const auto id = host.invoke({"usage-scan", version}, "scan",
            addon::encode_usage_scan_request(input, version), 30000);
        const auto outcome = host.wait_outcome(id, 31000);
        check(outcome.has_value(), "packaged scan must finish");
        return *outcome;
    };
    const auto page_for = [&](const addon::Usage_scan_request& input, std::uint16_t version) {
        const auto outcome = outcome_for(input, version);
        check(outcome.status == addon::Host_status::SUCCESS,
            "packaged scan failed: " + outcome.reason_code + "; request=" +
                addon::serialize_json(addon::encode_usage_scan_request(input, version)));
        const auto page = addon::decode_usage_scan_result(outcome.payload, &failure, version);
        check(page.has_value(), failure.detail);
        if (version == 3) {
            check(page->profile_storage && input.context.profile_storage &&
                page->profile_storage->version == input.context.profile_storage->version &&
                page->profile_storage->root == input.context.profile_storage->root,
                "every usage3 page echoes the exact requested profile storage");
            check(page->account_context.has_value() == input.context.account_context.has_value(),
                "usage3 preserves account-context presence without synthesizing it");
            if (input.context.account_context) {
                check(addon::encode_account_context(*page->account_context) ==
                    addon::encode_account_context(*input.context.account_context),
                    "every usage3 page preserves account version, key and profile exactly");
            }
            else {
                check(!outcome.payload.find("account_context"),
                    "absent account context is omitted on the wire, not null");
            }
        }
        else {
            check(!outcome.payload.find("account_context") && !outcome.payload.find("profile_storage"),
                "legacy usage pages do not acquire usage3 acknowledgments");
        }
        return *page;
    };
    const auto all_pages = [&](addon::Usage_scan_request input, std::uint16_t version) {
        auto first = page_for(input, version);
        auto page = first;
        while (!page.complete) {
            input.snapshot_id = page.snapshot_id;
            input.cursor = page.next_cursor;
            page = page_for(input, version);
            check(page.cache_candidate == first.cache_candidate,
                "candidate descriptor stays identical across pages");
            first.records.insert(first.records.end(), page.records.begin(), page.records.end());
        }
        return first;
    };
    const auto total = [](const addon::Usage_scan_page& page) {
        std::uint64_t sum = 0;
        for (const auto& record : page.records) {
            sum += record.total_tokens;
        }
        return sum;
    };
    const auto select = [&](std::string name, std::optional<addon::Usage_cache_reference> accepted) {
        request.snapshot_id.reset();
        request.cursor.reset();
        request.mode = "incremental";
        auto parent = storage;
        while (parent.native().size() < 291) {
            parent /= "retained-account-cache-allocation-component";
        }
        const auto path = parent / name;
        check(fs::create_directories(addon::usage_cache_io_path(path)),
            "host exclusively allocates long candidate directory");
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(path)};
    };
    const auto validate = [&](const addon::Usage_cache_reference& reference) {
        check(addon::validate_usage_cache_candidate(reference, *allocated, request.context.account_context,
            [] { return false; }, &failure).has_value(), "sealed candidate remains intact: " + failure.detail);
    };

    check(total(all_pages(request, 2)) == 2700, "v2 retains its existing namespace and telemetry");
    for (const auto& path : sources) {
        check(fs::remove(path), "remove fixture source");
    }
    select("empty-selection", std::nullopt);
    const auto empty = all_pages(request, 3);
    check(empty.records.empty() && empty.cache_candidate,
        "absent host selection never bootstraps from mutable provider cache");
    validate(*empty.cache_candidate);

    sources = write_sources(profile, 10);
    select("accepted", std::nullopt);
    const auto first_request = request;
    const auto first = page_for(request, 3);
    check(!first.complete && first.records.size() == 256 && first.cache_candidate,
        "packaged first page supplies a sealed bounded candidate");
    validate(*first.cache_candidate);
    request.snapshot_id = first.snapshot_id;
    request.cursor = first.next_cursor;
    auto changed = request;
    changed.mode = "repair";
    check(outcome_for(changed, 3).status != addon::Host_status::SUCCESS,
        "continuation cannot change the frozen request");
    const auto last = page_for(request, 3);
    check(last.complete && last.records.size() == 14 && last.cache_candidate == first.cache_candidate,
        "packaged final page preserves candidate identity");
    for (const auto& record : first.records) {
        check(record.aggregate_evidence == "partial", "surviving native subsets make conservative evidence claims");
    }

    select("account-v1", std::nullopt);
    request.context.account_context->version = 1;
    request.context.account_context->profile_path.reset();
    // Version one has no profile locator; acknowledgment support must not
    // bypass the native provider's existing explicit-account admission.
    const auto version_one = outcome_for(request, 3);
    check(version_one.status != addon::Host_status::SUCCESS &&
        version_one.reason_code == "usage_source_unavailable",
        "version-one account without a profile locator retains its explicit refusal");
    request.context = first_request.context;
    validate(*first.cache_candidate);

    select("account-absent", std::nullopt);
    request.context.account_context.reset();
    const auto account_absent = all_pages(request, 3);
    check(account_absent.records.size() == 270 && total(account_absent) == 2700,
        "environment-selected profile keeps account context absent on every page");
    request.context = first_request.context;

    sources = write_sources(profile, 1);
    select("unaccepted", first.cache_candidate);
    request.mode = "repair";
    const auto lower = all_pages(request, 3);
    check(total(lower) == 270 && lower.cache_candidate, "lower native observation is a separate candidate");
    validate(*first.cache_candidate);
    for (const auto& path : sources) {
        check(fs::remove(path), "remove refreshed fixture source");
    }

    // The host rejects the lower partial aggregate and continues selecting the coherent original cache.
    select("recovered", first.cache_candidate);
    const auto recovered = all_pages(request, 3);
    check(total(recovered) == 2700, "rejected candidate cannot become retained history after source deletion");
    validate(*first.cache_candidate);
    validate(*lower.cache_candidate);

    select("cancelled", first.cache_candidate);
    Cancelled_context cancelled(request);
    bool saw_cancel = false;
    try {
        qwen::handle_usage(cancelled);
    }
    catch (const std::runtime_error& error) {
        saw_cancel = std::string(error.what()) == "cancelled";
    }
    check(saw_cancel, "cancelled scan cannot seal or promote a candidate");
    check(!fs::exists(addon::path_from_utf8(request.cache_selection->candidate_root) / "cache-manifest.json"),
        "cancelled candidate stays unsealed");
    validate(*first.cache_candidate);

    select("reset", std::nullopt);
    request.mode = "reset";
    const auto reset = all_pages(request, 3);
    check(reset.records.empty() && reset.cache_candidate, "reset reconstructs only current sources");
    validate(*reset.cache_candidate);
    validate(*first.cache_candidate);
    auto invalid_reset = request;
    invalid_reset.cache_selection->accepted = first.cache_candidate;
    check(outcome_for(invalid_reset, 3).status != addon::Host_status::SUCCESS,
        "reset cannot import a retained selection");

    auto old_request = first_request;
    old_request.cache_selection.reset();
    check(total(all_pages(old_request, 2)) == 2700, "v3 scans and reset leave v2 cache untouched");

    select("occupied", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root) / "foreign"),
        "occupied");
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "provider refuses a nonempty host allocation");
    validate(*first.cache_candidate);

    const auto mutable_path = storage / "qwen-usage-v1";
    const auto conflicting = mutable_path / "host-candidate";
    if (!fs::exists(mutable_path) || fs::is_directory(mutable_path)) {
        fs::create_directories(conflicting);
    }
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, addon::path_to_utf8(conflicting)};
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "host candidates cannot enter the provider mutable namespace");

    select("wrong-account", first.cache_candidate);
    auto wrong_account = request;
    const auto other_profile = fixture / "other-native-profile";
    fs::create_directories(other_profile);
    wrong_account.context.account_context = addon::Account_context{
        2, qwen::account_key(other_profile), addon::path_to_utf8(other_profile)};
    wrong_account.context.launch_environment->bootstrap =
        addon::Environment_bootstrap_evidence{addon::path_to_utf8(other_profile)};
    check(outcome_for(wrong_account, 3).status != addon::Host_status::SUCCESS,
        "selected cache cannot cross native profile and account ownership");
    validate(*first.cache_candidate);

    select("corrupt-input", first.cache_candidate);
    write(addon::usage_cache_io_path(addon::path_from_utf8(first.cache_candidate->root) / "unlisted"), "unexpected");
    check(outcome_for(request, 3).status != addon::Host_status::SUCCESS,
        "corrupt selected inventory fails without fallback");
    check(host.shutdown(1000, nullptr), "packaged adapter exits without contacting a model");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "expected built package directory");
        Workspace workspace;
        adapter_cache_contract(addon::path_from_utf8(argv[1]), workspace.m_path);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
