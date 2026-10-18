#include "qwen_profile.h"
#include "qwen_usage.h"

#include <logonomic_cli_addon/environment_bootstrap.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
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

std::string read_bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string transcript_row(const std::string& session, const std::string& uuid, unsigned tokens)
{
    return "{\"sessionId\":\"" + session + "\",\"uuid\":\"" + uuid + "\","
        "\"timestamp\":\"2026-09-11T23:59:59.125Z\",\"cwd\":\"\",\"type\":\"system\","
        "\"subtype\":\"ui_telemetry\",\"systemPayload\":{\"uiEvent\":{"
        "\"event.name\":\"qwen-code.api_response\",\"model\":\"qwen3\","
        "\"input_token_count\":" + std::to_string(tokens) + ",\"output_token_count\":0,"
        "\"cached_content_token_count\":0,\"thoughts_token_count\":0,\"total_token_count\":" +
        std::to_string(tokens) + "}}}\n";
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
    const auto transcript = profile / "projects/repo/chats/session-0.jsonl";
    write(transcript, transcript_row("session-0", "original-call", tokens));
    return {path, transcript};
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
    auto* active_host = &host;
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
        const auto id = active_host->invoke({"usage-scan", version}, "scan",
            addon::encode_usage_scan_request(input, version), 30000);
        const auto outcome = active_host->wait_outcome(id, 31000);
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
        check(record.temporal && record.temporal->parent.record_id == record.record_id &&
            record.temporal->parent.total_tokens == record.total_tokens &&
            record.temporal->parent.snapshot_id == first.snapshot_id &&
            record.temporal->parent.source_account_key == first.scope.source_account_key,
            "optional proof binds actual package accounting snapshot and account");
        if (record.record_id == "qwen:session-0") {
            check(record.temporal->portions.size() == 1 &&
                record.temporal->portions[0].instant == "2026-09-11T23:59:59.125Z" &&
                record.temporal->portions[0].total_tokens == 10,
                "packaged proof preserves native timestamp rather than summary completion date");
        }
    }

    // Native size/time identity is the incremental oracle. Repair deliberately
    // rereads even a correction that preserves both, across an adapter restart.
    std::vector<fs::file_time_type> original_times;
    std::vector<std::uintmax_t> original_sizes;
    for (const auto& path : sources) {
        original_times.push_back(fs::last_write_time(path));
        original_sizes.push_back(fs::file_size(path));
    }
    sources = write_sources(profile, 20);
    for (std::size_t index = 0; index < sources.size(); ++index) {
        check(fs::file_size(sources[index]) == original_sizes[index], "same-size native correction fixture");
        fs::last_write_time(sources[index], original_times[index]);
    }
    check(host.shutdown(1000, nullptr), "stop original adapter before durable incremental replay");
    addon::Host_simulator restarted_host(options, nullptr);
    check(restarted_host.start(&failure), failure.detail);
    active_host = &restarted_host;
    const auto restarted_ack = active_host->invoke({"profile-storage", 1}, "acknowledge",
        addon::Json_value::object(), 5000, invocation);
    const auto restarted_acknowledged = active_host->wait_outcome(restarted_ack, 6000);
    check(restarted_acknowledged && restarted_acknowledged->status == addon::Host_status::SUCCESS,
        "restarted adapter acknowledges the retained host allocation");
    select("unchanged-restarted", first.cache_candidate);
    const auto unchanged = all_pages(request, 3);
    check(total(unchanged) == 2700 && !unchanged.incomplete_historical_coverage,
        "incremental replay reuses unchanged source identities and preserves complete source coverage");
    select("repair-unchanged", unchanged.cache_candidate);
    request.mode = "repair";
    check(total(all_pages(request, 3)) == 5400,
        "full repair rereads native corrections despite unchanged size and timestamp");

    const auto original_records_path = addon::usage_cache_io_path(
        addon::path_from_utf8(first.cache_candidate->root) / "records");
    for (const auto version : {0, 999}) {
        select("index-upgrade-input-" + std::to_string(version), std::nullopt);
        const auto input_root = request.cache_selection->candidate_root;
        const auto input_path = addon::usage_cache_io_path(addon::path_from_utf8(input_root));
        write(input_path / "records", read_bytes(original_records_path));
        if (version != 0) {
            write(input_path / "source-index", "{\"version\":999}\n");
        }
        const auto sealed = addon::seal_usage_cache_candidate(input_root, *allocated,
            request.context.account_context, "index-upgrade", "qwen-evidence-cache@1",
            [] { return false; }, &failure);
        check(sealed.has_value(), "seal optional-index upgrade fixture: " + failure.detail);
        select("index-upgrade-output-" + std::to_string(version), sealed);
        const auto upgraded = all_pages(request, 3);
        check(total(upgraded) == 5400,
            "absent or unknown optional index rereads native sources without losing accepted accounting");
    }

    sources = write_sources(profile, 10);
    const auto added_source = profile / "projects/repo/chats/incremental-new.jsonl";
    const auto original_event = transcript_row("incremental-new", "first-call", 7);
    write(added_source, original_event);
    fs::last_write_time(added_source, original_times.front() - std::chrono::hours(24));
    select("backdated-new", first.cache_candidate);
    const auto discovered = all_pages(request, 3);
    check(total(discovered) == 2707, "incremental discovery includes new files older than the previous scan");
    const auto appended_events = original_event + transcript_row("incremental-new", "second-call", 5);
    write(added_source, appended_events);
    select("appended", discovered.cache_candidate);
    const auto appended = all_pages(request, 3);
    check(total(appended) == 2712, "incremental update reconciles appended events exactly once");
    select("appended-unchanged", appended.cache_candidate);
    check(total(all_pages(request, 3)) == 2712, "unchanged replay does not count the append again");
    const auto duplicate_source = profile / "projects/repo/chats/duplicate.jsonl";
    write(duplicate_source, original_event);
    select("duplicate-live", appended.cache_candidate);
    check(outcome_for(request, 3).reason_code == "usage_session_conflict",
        "reused sources still participate in duplicate live session detection");
    check(fs::remove(duplicate_source), "remove duplicate source fixture");
    check(fs::remove(added_source), "remove accepted source fixture");
    select("deleted-incremental", appended.cache_candidate);
    const auto deleted = all_pages(request, 3);
    check(total(deleted) == 2712 && deleted.incomplete_historical_coverage,
        "incremental update retains accepted events after source deletion");
    select("deleted-repair", deleted.cache_candidate);
    request.mode = "repair";
    check(total(all_pages(request, 3)) == 2712, "full repair preserves accepted history with deleted sources");

    write(added_source, original_event + "{\"sessionId\"");
    select("partial-source", first.cache_candidate);
    const auto partial = all_pages(request, 3);
    check(total(partial) == 2707 && partial.incomplete_historical_coverage,
        "partial source retains its complete observed event");
    const auto partial_index = read_bytes(addon::usage_cache_io_path(
        addon::path_from_utf8(partial.cache_candidate->root) / "source-index"));
    check(partial_index.find("incremental-new") == std::string::npos,
        "partial files cannot acquire a reusable complete-source marker");
    write(added_source, appended_events);
    select("partial-completed", partial.cache_candidate);
    check(total(all_pages(request, 3)) == 2712, "completed append recovers after partial input");
    check(fs::remove(added_source), "remove completed partial-source fixture");

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
    for (const auto& record : recovered.records) {
        check(record.temporal && record.temporal->retained_from &&
            record.temporal->retained_from->snapshot_id == first.cache_candidate->snapshot_id &&
            record.temporal->retained_from->manifest_sha256_hex == first.cache_candidate->manifest_sha256_hex,
            "retained proof binds the selected accepted manifest before publication");
        if (record.record_id == "qwen:session-0") {
            check(record.temporal->portions.size() == 1 &&
                record.temporal->portions[0].total_tokens == 10,
                "selected cache retains original event-time evidence with every native source gone");
        }
    }
    validate(*first.cache_candidate);
    validate(*lower.cache_candidate);

    // Oracle: optional carrier failure cannot change the admitted original
    // event grammar, footer, cache owner or summary accounting.
    const auto original_records = read_bytes(addon::usage_cache_io_path(
        addon::path_from_utf8(first.cache_candidate->root) / "records"));
    std::vector<std::string> causal_failures;
    for (const auto kind : {"absent", "malformed", "stale", "future", "oversized", "future-refresh", "deep"}) {
        std::istringstream input(original_records);
        std::string line;
        std::string replacement;
        addon::Sha256 digest;
        std::uint64_t count = 0;
        while (std::getline(input, line)) {
            auto row = addon::parse_json(line, {}, nullptr);
            check(row.has_value(), "accepted cache rows are parseable");
            if (row->find("end")) break;
            if (row->find("uuid")) {
                if (std::string_view(kind) == "absent") {
                    row->erase("temporal");
                }
                else
                if (std::string_view(kind) == "malformed") {
                    row->set("temporal", addon::Json_value("damaged optional carrier"));
                }
                else
                if (std::string_view(kind) == "stale") {
                    auto carrier = *row->find("temporal");
                    carrier.set("accounting_sha256", addon::Json_value(std::string(64, '0')));
                    row->set("temporal", std::move(carrier));
                }
                else {
                    auto opaque = addon::Json_value(std::string(
                        std::string_view(kind) == "oversized" ? 3 * 1024 * 1024 : 32, 'p'));
                    if (std::string_view(kind) == "deep") {
                        for (unsigned depth = 0; depth < 129; ++depth) {
                            auto nested = addon::Json_value::array();
                            nested.push_back(std::move(opaque));
                            opaque = std::move(nested);
                        }
                    }
                    const auto future = qwen::object({{"version", addon::Json_value(2)}, {"opaque", std::move(opaque)}});
                    row->set("temporal", future);
                }
            }
            line = addon::serialize_json(*row);
            replacement += line + '\n';
            digest.update(line);
            digest.update("\n");
            if (!row->find("owner")) ++count;
        }
        replacement += addon::serialize_json(qwen::object({{"end", addon::Json_value(1)},
            {"count", addon::Json_value(count)},
            {"sha256", addon::Json_value(addon::to_hex(digest.finish()))}})) + '\n';
        select(std::string("carrier-input-") + kind, std::nullopt);
        const auto input_root = request.cache_selection->candidate_root;
        write(addon::usage_cache_io_path(addon::path_from_utf8(input_root) / "records"), replacement);
        const auto sealed = addon::seal_usage_cache_candidate(input_root, *allocated,
            request.context.account_context, "carrier-fixture", "qwen-evidence-cache@1",
            [] { return false; }, &failure);
        check(sealed.has_value(), "seal optional-only carrier fixture: " + failure.detail);
        select(std::string("carrier-output-") + kind, sealed);
        addon::Usage_scan_page observed;
        try {
            observed = all_pages(request, 3);
        }
        catch (const std::runtime_error& error) {
            if (std::string_view(kind) != "deep") throw;
            causal_failures.push_back("over-depth optional carrier rejected original accounting: " + std::string(error.what()));
            continue;
        }
        check(total(observed) == 2700, "optional carrier cannot change original accounting");
        for (const auto& record : observed.records) {
            if (record.record_id == "qwen:session-0") {
                check(record.temporal && record.temporal->portions.empty() &&
                    record.temporal->unavailable_tokens == 10,
                    "unsupported absent damaged or stale carrier supplies no event-time proof");
            }
        }
        if (std::string_view(kind) == "future" || std::string_view(kind) == "oversized") {
            const auto bytes = read_bytes(addon::usage_cache_io_path(
                addon::path_from_utf8(observed.cache_candidate->root) / "records"));
            check(bytes.find("\"opaque\":") != std::string::npos,
                "bounded unknown optional carriers remain opaque through cache replay");
        }
        if (std::string_view(kind) == "future-refresh") {
            // Oracle: unchanged accounting cannot authorize deleting an opaque
            // future carrier; an actual counter correction cannot rebind it.
            sources = write_sources(profile, 10);
            select("carrier-live-refresh", observed.cache_candidate);
            const auto refreshed = all_pages(request, 3);
            const auto refreshed_bytes = read_bytes(addon::usage_cache_io_path(
                addon::path_from_utf8(refreshed.cache_candidate->root) / "records"));
            if (refreshed_bytes.find("\"opaque\":\"" + std::string(32, 'p') + "\"") == std::string::npos) {
                causal_failures.push_back("unchanged live UUID refresh erased its opaque future carrier");
            }
            for (const auto& path : sources) check(fs::remove(path), "remove unchanged live fixture source");
            select("carrier-after-live-deletion", refreshed.cache_candidate);
            const auto replayed = all_pages(request, 3);
            const auto replayed_bytes = read_bytes(addon::usage_cache_io_path(
                addon::path_from_utf8(replayed.cache_candidate->root) / "records"));
            if (replayed_bytes.find("\"opaque\":\"" + std::string(32, 'p') + "\"") == std::string::npos &&
                causal_failures.empty())
            {
                causal_failures.push_back("source deletion did not retain the opaque future carrier");
            }
            check(total(refreshed) == 2700 && total(replayed) == 2700,
                "live refresh and subsequent source deletion preserve original accounting");
            sources = write_sources(profile, 1);
            select("carrier-live-correction", replayed.cache_candidate);
            const auto corrected = all_pages(request, 3);
            const auto corrected_bytes = read_bytes(addon::usage_cache_io_path(
                addon::path_from_utf8(corrected.cache_candidate->root) / "records"));
            check(total(corrected) == 270 && corrected_bytes.find("\"opaque\":") == std::string::npos,
                "actual counter correction does not fabricate a binding for an unknown carrier");
            for (const auto& path : sources) check(fs::remove(path), "remove corrected live fixture source");
        }
        if (std::string_view(kind) == "deep") {
            const auto bytes = read_bytes(addon::usage_cache_io_path(
                addon::path_from_utf8(observed.cache_candidate->root) / "records"));
            check(bytes.find("\"opaque\":") == std::string::npos,
                "over-capacity optional depth is omitted without altering core framing");
        }
        validate(*first.cache_candidate);
    }
    if (!causal_failures.empty()) {
        for (const auto& message : causal_failures) std::cerr << message << '\n';
        throw std::runtime_error("packaged replay reproduced " + std::to_string(causal_failures.size()) + " review blockers");
    }

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
    check(active_host->shutdown(1000, nullptr), "packaged adapter exits without contacting a model");
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
