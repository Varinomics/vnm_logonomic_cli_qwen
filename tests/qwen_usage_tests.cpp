#include <logonomic_cli_addon/package_source.h>
#include "qwen_usage.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <source_location>
#include <stdexcept>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

#ifdef _WIN32
const std::string project = "C:/repo";
#else
const std::string project = "/repo";
#endif

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return result.has_value() || !failure.empty(); }
    void succeed(addon::Json_value value) override { result = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { failure = std::move(reason); }
    addon::Invoke_request invocation;
    std::optional<addon::Json_value> result;
    std::string failure;
    bool cancelled = false;
};

class Workspace final
{
public:
    Workspace() : path(fs::temp_directory_path() / ("qwen-usage-" + addon::random_nonce_hex())) { fs::create_directories(path); }
    ~Workspace() { std::error_code error; fs::remove_all(path, error); }
    fs::path path;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    require(static_cast<bool>(output), "fixture write failed");
}

std::string summary(std::string_view id, unsigned total)
{
    return "{\"version\":1,\"sessionId\":\"" + std::string(id) +
        "\",\"timestamp\":1789200000000,\"project\":\"" + project + "\",\"models\":{\"qwen3\":{\"requests\":3,\"inputTokens\":20,\"outputTokens\":10,\"cachedTokens\":0,\"totalTokens\":" +
        std::to_string(total) + "}}}\n";
}

std::string event(
    std::string_view id, std::string_view counts,
    std::string uuid = addon::random_nonce_hex(), std::string_view copied_from = {})
{
    const auto origin = copied_from.empty() ? std::string{} :
        ",\"forkedFrom\":{\"sessionId\":\"" + std::string(copied_from) + "\",\"messageUuid\":\"" + uuid + "\"}";
    return "{\"sessionId\":\"" + std::string(id) + "\",\"uuid\":\"" + uuid + "\"" + origin +
        ",\"timestamp\":\"2026-09-12T12:00:00.000Z\",\"cwd\":\"" + project + "\",\"type\":\"system\",\"subtype\":\"ui_telemetry\",\"systemPayload\":{\"uiEvent\":{\"event.name\":\"qwen-code.api_response\",\"model\":\"qwen3\",\"prompt_id\":\"" +
        std::string(id) + "#" + uuid + "\"," +
        std::string(counts) + "}}}\n";
}

const std::string counts = "\"input_token_count\":7,\"output_token_count\":5,\"cached_content_token_count\":0,\"thoughts_token_count\":2,\"total_token_count\":0";

std::string dated_event(std::string_view id, std::string_view values, std::string uuid,
    std::string_view instant, std::string_view copied_from = {})
{
    auto row = event(id, values, std::move(uuid), copied_from);
    row.replace(row.find("2026-09-12T12:00:00.000Z"), 24, instant);
    return row;
}

void native_temporal_uses_admitted_model_basis()
{
    // Oracle: the adopted extraction contract retains the native model-wide H
    // basis and proves only timestamps of admitted original UUID increments.
    Workspace workspace;
    Context context;
    const auto source = workspace.path / "projects/repo/chats/timed.jsonl";
    const auto first = "2026-09-11T23:59:59.125Z";
    const auto last  = "2026-09-12T00:00:00.250Z";
    const auto native = "\"input_token_count\":3,\"output_token_count\":2,\"thoughts_token_count\":1,\"total_token_count\":9";
    write(source, dated_event("timed", counts, "first", first) +
        dated_event("timed", native, "last", last));
    auto records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 9 && records[0].temporal &&
        records[0].temporal->portions.size() == 1 &&
        records[0].temporal->portions[0].instant == last &&
        records[0].temporal->portions[0].total_tokens == 9,
        "zero native event total does not invent per-event fallback in a positive-total model");
    const auto missing_total = "\"input_token_count\":3,\"output_token_count\":2,\"thoughts_token_count\":1";
    write(source, dated_event("timed", counts, "first", first) +
        dated_event("timed", missing_total, "last", last));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 20 && records[0].temporal &&
        records[0].temporal->portions.size() == 2 &&
        records[0].temporal->portions[0].instant == first &&
        records[0].temporal->portions[0].total_tokens == 14 &&
        records[0].temporal->portions[1].total_tokens == 6,
        "missing model total uses the same admitted input output thoughts fallback for every event");
    write(workspace.path / "usage_record.jsonl", summary("timed", 30));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 30 && records[0].temporal &&
        records[0].temporal->unavailable_tokens == 10 &&
        records[0].temporal->coverage_reason == "missing-evidence",
        "summary H retains exact partial transcript proof independently of completion date");
    write(workspace.path / "usage_record.jsonl", summary("timed", 10));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 10 && records[0].temporal &&
        records[0].temporal->portions.empty() && records[0].temporal->unavailable_tokens == 10,
        "transcript total above summary H withdraws only optional temporal proof");
    fs::remove(source);
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 10 && records[0].temporal &&
        records[0].temporal->portions.empty() && records[0].temporal->unavailable_tokens == 10,
        "summary-only completion date is not an event instant");

    fs::remove(workspace.path / "usage_record.jsonl");
    write(source, dated_event("timed", native, "same", last) +
        dated_event("timed", native, "same", "2026-09-12T00:00:01.250Z"));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 9 && records[0].temporal &&
        records[0].temporal->portions.empty() && records[0].temporal->unavailable_tokens == 9,
        "same-accounting duplicate timestamp disagreement withdraws temporal proof only");
    write(source, dated_event("timed", native, "same", last));
    const auto base_size = addon::serialize_json(addon::encode_stored_usage_accounting(
        qwen::scan_usage(workspace.path, context)[0])).size();
    qwen::Usage_limits limits;
    limits.result_bytes = base_size;
    records = qwen::scan_usage(workspace.path, context, limits);
    require(records.size() == 1 && records[0].total_tokens == 9 && !records[0].temporal,
        "original result bytes take priority over optional envelope bytes");
    write(source, dated_event("session\\twith-tab", native, "same", last));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 9 && !records[0].temporal,
        "accounting identity containing an admitted control character omits optional temporal proof");
    for (const auto invalid : {"2026-09-12T00:00:00+00:00", "2026-09-12T00:00:00.250000000Z",
        "2026-09-12T00:00:00", "2026-09-12T00:00:60.250Z"})
    {
        write(source, dated_event("timed", native, "same", invalid));
        bool rejected = false;
        try { (void)qwen::scan_usage(workspace.path, context); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "native transcript timestamp admission stays Date.toISOString millisecond UTC");
    }
}

void native_recovery_and_summary_precedence()
{
    Workspace workspace;
    const auto home = workspace.path / "profile";
    const auto runtime = workspace.path / "runtime";
    write(home / "usage_record.jsonl", summary("retained", 31) + summary("retained", 32) + "{\"version\":");
    write(home / "projects/repo/chats/retained.jsonl", event("retained", counts));
    write(home / "projects/repo/chats/live.jsonl", event("live", counts) + event("live", counts));
    write(home / "projects/repo/chats/nested/ignored.jsonl", event("nested", counts));
    write(home / "projects/repo/chats/ignored.ledger.jsonl", event("ledger", counts));
    write(runtime / "projects/repo/chats/ignored.jsonl", event("runtime", counts));
    write(home / "projects/repo/chats/assistant.jsonl",
        "{\"sessionId\":\"assistant\",\"timestamp\":\"2026-09-12T12:00:00.000Z\",\"type\":\"assistant\",\"usageMetadata\":{\"totalTokenCount\":999}}\n");
    Context context;
    const auto records = qwen::scan_usage(home, context);
    require(records.size() == 2, "only native global chats telemetry and durable summaries contribute");
    require(records[0].record_id == "qwen:live" && records[0].total_tokens == 28, "native per-model zero-total fallback includes thoughts once");
    require(records[0].input.value == 14 && records[0].input.coverage->covered_records == 2 &&
        records[0].input.coverage->total_records == 2, "replayed API response coverage counts actual events");
    require(records[0].cache_read.value == 0, "observed zero cache tokens stay known");
    require(records[0].source_counter_semantics == "per_event_sum", "transcript counter provenance is truthful");
    require(records[1].total_tokens == 32 && records[1].source_counter_semantics == "cumulative_final", "durable last-wins summary remains authoritative");
    require(!records[1].cache_creation.applicable, "unavailable native cache-creation field remains unsupported");
}

void partial_categories_and_torn_recovery()
{
    Workspace workspace;
    const auto path = workspace.path / "projects/repo/chats/live.jsonl";
    const auto partial = "\"input_token_count\":3,\"output_token_count\":2,\"thoughts_token_count\":0,\"total_token_count\":5";
    auto concatenated = event("live", counts);
    concatenated.pop_back();
    write(path, concatenated + event("live", partial) + "{\"unfinished\":");
    Context context;
    const auto records = qwen::scan_usage(workspace.path, context);
    require(records.size() == 1 && records[0].total_tokens == 5, "native nonzero accumulated total overrides the per-model fallback");
    require(records[0].cache_read.value == 0 && records[0].cache_read.coverage->covered_records == 1 &&
        records[0].cache_read.coverage->total_records == 2 && records[0].cache_read.coverage->support == addon::Usage_support::UNKNOWN,
        "known zero survives partial category coverage without claiming complete evidence");
}

void select_next_cache(addon::Usage_scan_request& request,
    std::optional<addon::Usage_cache_reference> accepted)
{
    const auto root = addon::path_from_utf8(request.context.profile_storage->root);
    const auto candidate = root / addon::random_nonce_hex();
    require(fs::create_directory(candidate), "allocate an exclusive current cache candidate");
    request.snapshot_id.reset();
    request.cursor.reset();
    request.cache_selection = addon::Usage_cache_selection{std::move(accepted), qwen::path_string(candidate)};
}

addon::Usage_scan_request request_for(const fs::path& home)
{
    addon::Usage_scan_request request;
    request.context.launcher = {"qwen", "direct", ""};
    request.context.launcher_argv = {"qwen"};
    request.context.working_directory = qwen::path_string(home);
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment = json_util::object({{"QWEN_HOME", addon::Json_value(qwen::path_string(home))}});
    environment.bootstrap = addon::Environment_bootstrap_evidence{qwen::path_string(home)};
    request.context.launch_environment = std::move(environment);
    const auto storage = home / "package-storage";
    fs::create_directories(storage);
    request.context.profile_storage = addon::Profile_storage_context{1, qwen::path_string(fs::canonical(storage))};
    request.context.account_context = addon::Account_context{2, qwen::account_key(home), qwen::path_string(home)};
    select_next_cache(request, std::nullopt);
    return request;
}

Context invoke(const addon::Usage_scan_request& request)
{
    Context context;
    context.invocation.capability = {"usage-scan", 3};
    context.invocation.operation = "scan";
    context.invocation.payload = addon::encode_usage_scan_request(request, 3);
    qwen::handle_usage(context);
    return context;
}

void pages_bind_exact_environment_and_survive_retry()
{
    Workspace workspace;
    std::string history;
    for (unsigned index = 0; index != 257; ++index) history += summary("session-" + std::to_string(index), index);
    write(workspace.path / "usage_record.jsonl", history + "{\"partial\":");
    auto request = request_for(workspace.path);
    auto first = invoke(request);
    require(first.result.has_value(), "first page succeeds");
    addon::Contract_failure failure;
    const auto page = addon::decode_usage_scan_result(*first.result, &failure, 3);
    require(page && !page->complete && page->records.size() == 256 && page->incomplete_historical_coverage, "bounded first page reports recovered torn input");
    request.snapshot_id = page->snapshot_id;
    request.cursor = page->next_cursor;
    auto changed = request;
    changed.context.launch_environment->effective_environment.set("QWEN_RUNTIME_DIR", addon::Json_value("changed"));
    require(invoke(changed).failure == "usage_snapshot_expired", "same account cannot page a snapshot under a changed frozen environment");
    const auto last = invoke(request);
    const auto retry = invoke(request);
    require(last.result && retry.result && addon::serialize_json(*last.result) == addon::serialize_json(*retry.result), "completed final page is immutable and retryable");
    request.context.launch_environment->bootstrap.reset();
    require(invoke(request).failure == "environment_bootstrap_unavailable", "every page requires host bootstrap evidence");
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

addon::Usage_scan_page successful_page(
    addon::Usage_scan_request& request,
    std::source_location location = std::source_location::current())
{
    const auto result = invoke(request);
    if (!result.result) {
        throw std::runtime_error(std::string(location.function_name()) + ":" +
            std::to_string(location.line()) + ": usage scan unexpectedly failed: " + result.failure);
    }
    addon::Contract_failure failure;
    const auto page = addon::decode_usage_scan_result(*result.result, &failure, 3);
    require(page && page->complete && page->cache_candidate, "usage page violates public codec");
    // The fixture accepts this complete candidate and selects it explicitly
    // for its next scan, which always receives a fresh exclusive allocation.
    select_next_cache(request, page->cache_candidate);
    return *page;
}

std::string token_counts(unsigned tokens)
{
    return "\"input_token_count\":" + std::to_string(tokens) +
        ",\"output_token_count\":0,\"cached_content_token_count\":0,\"thoughts_token_count\":0,\"total_token_count\":" +
        std::to_string(tokens);
}

std::string telemetry_summary(std::string_view id, unsigned tokens, unsigned requests)
{
    return "{\"version\":1,\"sessionId\":\"" + std::string(id) +
        "\",\"timestamp\":1789200000000,\"project\":\"" + project +
        "\",\"models\":{\"qwen3\":{\"requests\":" + std::to_string(requests) +
        ",\"inputTokens\":" + std::to_string(tokens) +
        ",\"outputTokens\":0,\"cachedTokens\":0,\"totalTokens\":" + std::to_string(tokens) + "}}}\n";
}

std::uint64_t tokens_for(const addon::Usage_scan_page& page, std::string_view id)
{
    for (const auto& record : page.records) {
        if (record.record_id == "qwen:" + std::string(id)) {
            require(record.session_key == record.record_id, "public identity remains the native session");
            return record.total_tokens;
        }
    }
    throw std::runtime_error("expected session missing from usage page");
}

void partial_sources_preserve_evidence_and_clean_corrections()
{
    // Current usage distinguishes missing source evidence from an observed
    // correction. Neither scan mode nor the incomplete-history flag erases it.
    Workspace workspace;
    auto request = request_for(workspace.path);
    const auto history = workspace.path / "usage_record.jsonl";
    write(history, telemetry_summary("summary", 100, 2));
    require(tokens_for(successful_page(request), "summary") == 100, "accept complete summary");
    write(history, telemetry_summary("summary", 40, 1) + "{\"version\":1,");
    const auto partial = successful_page(request);
    require(tokens_for(partial, "summary") == 100 && partial.incomplete_historical_coverage,
        "torn summary cannot overwrite retained same-session total");
    write(history, telemetry_summary("summary", 40, 1));
    require(tokens_for(successful_page(request), "summary") == 40,
        "clean authoritative summary can correct downward without a monotonic maximum");

    const auto transcript = workspace.path / "projects/repo/chats/live.jsonl";
    write(transcript, event("live", token_counts(60), "first-call") +
        event("live", token_counts(40), "second-call"));
    require(tokens_for(successful_page(request), "live") == 100, "accept both observed UUIDs");
    write(transcript, event("live", token_counts(40), "second-call") + "{\"unfinished\":");
    require(tokens_for(successful_page(request), "live") == 100,
        "torn transcript keeps accepted UUID absent from recovered prefix");
    write(history, telemetry_summary("summary", 30, 1));
    const auto local_authority = successful_page(request);
    require(tokens_for(local_authority, "summary") == 30 && tokens_for(local_authority, "live") == 100,
        "an unrelated torn transcript does not prevent a clean summary's downward correction");
    write(history, telemetry_summary("summary", 40, 1) + telemetry_summary("live", 40, 1) + "{");
    require(tokens_for(successful_page(request), "live") == 100,
        "new partial summary cannot replace previously published transcript evidence");
    fs::remove(history);
    write(transcript, event("live", token_counts(40), "second-call"));
    const auto pruned = successful_page(request);
    require(tokens_for(pruned, "live") == 100 && pruned.incomplete_historical_coverage,
        "clean file pruning is absence, not a counter correction");
    write(transcript, event("live", token_counts(20), "first-call"));
    require(tokens_for(successful_page(request), "live") == 60,
        "observed UUID counter correction replaces that event while retaining the missing sibling");
    fs::remove(transcript);
    require(tokens_for(successful_page(request), "live") == 60,
        "fresh selected-cache reload preserves event-level evidence after source deletion");
}

void forks_count_unique_calls_with_summaries_and_deleted_parents()
{
    // sessionService::forkSession preserves uuid, stamps immediate forkedFrom,
    // and rewrites prompt_id. A copied response is the original spend; only a
    // newly emitted response adds spend. Native salvage summaries sum both.
    for (const bool reverse : {false, true}) {
        Workspace workspace;
        auto request = request_for(workspace.path);
        const auto chats = workspace.path / "projects/repo/chats";
        const auto parent = chats / (reverse ? "z-parent.jsonl" : "a-parent.jsonl");
        const auto branch = chats / (reverse ? "a-branch.jsonl" : "z-branch.jsonl");
        const auto history = workspace.path / "usage_record.jsonl";
        write(parent, event("parent", token_counts(100), "parent-call"));
        write(branch, event("branch", token_counts(100), "parent-call", "parent") +
            event("branch", token_counts(25), "branch-call"));
        write(history, telemetry_summary("parent", 100, 1) + telemetry_summary("branch", 125, 2));
        const auto initial = successful_page(request);
        require(initial.records.size() == 2 && tokens_for(initial, "parent") == 100 &&
            tokens_for(initial, "branch") == 25,
            "summaries-first input still counts parent and copied UUID once, plus new fork spend");
        fs::remove(parent);
        write(history, telemetry_summary("branch", 125, 2));
        const auto retained = successful_page(request);
        require(tokens_for(retained, "parent") == 100 && tokens_for(retained, "branch") == 25 &&
            retained.incomplete_historical_coverage,
            "parent transcript and summary deletion does not change unique retained spend");

        write(chats / "nested.jsonl", event("nested", token_counts(100), "parent-call", "branch") +
            event("nested", token_counts(25), "branch-call", "branch") +
            event("nested", token_counts(7), "nested-call"));
        write(history, telemetry_summary("branch", 125, 2) + telemetry_summary("nested", 132, 3));
        const auto nested = successful_page(request);
        require(nested.records.size() == 3 && tokens_for(nested, "parent") == 100 &&
            tokens_for(nested, "branch") == 25 && tokens_for(nested, "nested") == 7,
            "nested immediate copy links resolve to the original UUID owner independently of filename order");
        const auto cache = addon::path_from_utf8(request.cache_selection->accepted->root) / "records";
        const auto accepted = read(cache);
        write(branch, event("branch", token_counts(99), "parent-call", "parent") +
            event("branch", token_counts(25), "branch-call"));
        require(invoke(request).failure == "usage_event_conflict" && read(cache) == accepted,
            "contradictory retained copies fail without mutating the accepted evidence");
        write(branch, event("branch", token_counts(100), "parent-call", "parent") +
            event("branch", token_counts(25), "branch-call"));
        write(history, telemetry_summary("branch", 200, 2));
        require(invoke(request).failure == "usage_fork_partition_unavailable" && read(cache) == accepted,
            "unpartitionable known-fork summary fails instead of inventing or duplicating spend");
    }
}

void copied_evidence_recovers_ancestry_without_parent_files()
{
    Workspace workspace;
    auto request = request_for(workspace.path);
    const auto chats = workspace.path / "projects/repo/chats";
    write(chats / "nested.jsonl", event("nested", token_counts(100), "parent-call", "branch") +
        event("nested", token_counts(25), "branch-call", "branch") +
        event("nested", token_counts(7), "nested-call"));
    const auto recovered = successful_page(request);
    require(tokens_for(recovered, "branch") == 125 && tokens_for(recovered, "nested") == 7 &&
        recovered.incomplete_historical_coverage,
        "copied evidence survives even when the immediate parent was never locally observed");
    write(chats / "branch.jsonl", event("branch", token_counts(100), "parent-call", "parent") +
        event("branch", token_counts(25), "branch-call"));
    const auto resolved = successful_page(request);
    require(tokens_for(resolved, "parent") == 100 && tokens_for(resolved, "branch") == 25 &&
        tokens_for(resolved, "nested") == 7,
        "stronger ancestry corrects the existing stable session amount instead of leaving an orphan total");
    fs::remove(chats / "nested.jsonl");
    fs::remove(chats / "branch.jsonl");
    const auto retained = successful_page(request);
    require(tokens_for(retained, "parent") == 100 && tokens_for(retained, "branch") == 25 &&
        tokens_for(retained, "nested") == 7, "cache reload preserves the resolved copy graph with every source gone");

    Workspace copy_only;
    auto copy_request = request_for(copy_only.path);
    const auto copy_chats = copy_only.path / "projects/repo/chats";
    write(copy_chats / "nested.jsonl", event("nested", token_counts(100), "shared-call", "branch"));
    require(tokens_for(successful_page(copy_request), "branch") == 100,
        "unobserved immediate ancestor owns the recovered copy until stronger evidence arrives");
    write(copy_chats / "branch.jsonl", event("branch", token_counts(100), "shared-call", "parent"));
    const auto zeroed = successful_page(copy_request);
    require(tokens_for(zeroed, "parent") == 100 && tokens_for(zeroed, "branch") == 0 &&
        tokens_for(zeroed, "nested") == 0,
        "superseded attribution publishes explicit zero under the earlier stable session key");
}

void temporal_bounds_preserve_original_rows_and_copy_owners()
{
    Workspace workspace;
    Context context;
    const auto chats = workspace.path / "projects/repo/chats";
    const auto instant = "2026-09-12T12:00:00.125Z";
    write(chats / "parent.jsonl", dated_event("parent", token_counts(10), "original", instant));
    write(chats / "branch.jsonl", dated_event("branch", token_counts(10), "original", instant, "parent") +
        dated_event("branch", token_counts(3), "branch-call", instant));
    auto records = qwen::scan_usage(workspace.path, context);
    require(records.size() == 2 && records[0].record_id == "qwen:branch" &&
        records[0].total_tokens == 3 && records[0].temporal->portions[0].total_tokens == 3 &&
        records[1].total_tokens == 10 && records[1].temporal->portions[0].total_tokens == 10,
        "temporal increments follow original UUID copy ownership before reduction");
    write(chats / "branch.jsonl", dated_event("branch", token_counts(10), "original",
        "2026-09-12T12:00:00.250Z", "parent") + dated_event("branch", token_counts(3), "branch-call", instant));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 3 && records[0].temporal->unavailable_tokens == 0 &&
        records[1].total_tokens == 10 && records[1].temporal->unavailable_tokens == 10,
        "copied timestamp disagreement withdraws only its original owner proof");
    fs::remove(chats / "parent.jsonl");
    fs::remove(chats / "branch.jsonl");
    const auto dense = [&](std::string id, unsigned count) {
        std::string history;
        for (unsigned index = 0; index < count; ++index) {
            const auto timestamp = "2026-09-12T12:00:0" + std::to_string(index / 1000) + '.' +
                std::to_string(1000 + index % 1000).substr(1) + 'Z';
            history += dated_event(id, token_counts(1), id + '-' + std::to_string(index), timestamp);
        }
        return history;
    };
    write(chats / "dense.jsonl", dense("dense", 4097));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].total_tokens == 4097 && records[0].temporal &&
        records[0].temporal->portions.empty() && records[0].temporal->unavailable_tokens == 4097 &&
        records[0].temporal->coverage_reason == "capacity",
        "position capacity withdraws optional proof while conserving original H");
    write(chats / "dense.jsonl", dense("dense", 4096));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].temporal && records[0].temporal->portions.size() == 4096 &&
        records[0].temporal->unavailable_tokens == 0,
        "the full admitted 4096-position boundary retains exact proof");
    qwen::Usage_limits limits;
    limits.result_bytes = addon::serialize_json(addon::encode_stored_usage_accounting(records[0])).size() + 4096;
    records = qwen::scan_usage(workspace.path, context, limits);
    require(records[0].total_tokens == 4096 && records[0].temporal &&
        records[0].temporal->portions.empty() && records[0].temporal->coverage_reason == "capacity",
        "reduced original result budget admits accounting and a validated capacity envelope");
    fs::remove(chats / "dense.jsonl");
    for (unsigned index = 0; index < 8; ++index) {
        const auto id = "dense-" + std::to_string(index);
        write(chats / (id + ".jsonl"), dense(id, 4096));
    }
    auto request = request_for(workspace.path);
    const auto candidate = addon::path_from_utf8(request.context.profile_storage->root) / "candidate";
    fs::create_directories(candidate);
    request.cache_selection = addon::Usage_cache_selection{std::nullopt, qwen::path_string(candidate)};
    const auto page = successful_page(request);
    require(page.complete && page.records.size() == 8 &&
        addon::serialize_json(addon::encode_usage_scan_result(page, 3)).size() <= 2 * 1024 * 1024,
        "actual metadata and cursor framing fit Qwen's complete page bound");
    bool capacity = false;
    for (const auto& record : page.records) {
        require(record.total_tokens == 4096 && record.temporal &&
            record.temporal->parent.snapshot_id == page.snapshot_id,
            "page fallback preserves every original row and actual temporal parent");
        capacity = capacity || record.temporal->coverage_reason == "capacity";
    }
    require(capacity, "page boundary reduces optional proof without skipping the accounting cursor");
}

void current_scans_refresh_durable_history()
{
    Workspace workspace;
    auto request = request_for(workspace.path);
    const auto source = workspace.path / "usage_record.jsonl";
    write(source, summary("retained", 30));
    const auto first = successful_page(request);
    require(first.records[0].total_tokens == 30 &&
        fs::is_regular_file(addon::path_from_utf8(first.cache_candidate->root) / "records"),
        "first successful page already has durable session evidence");
    write(source, summary("retained", 40));
    require(successful_page(request).records[0].total_tokens == 40,
        "current scans continue updating durable evidence after first scan");
    fs::remove(source);
    const auto retained = successful_page(request); // New scan reads disk, never an in-memory continuation.
    require(retained.records.size() == 1 && retained.records[0].total_tokens == 40 && retained.incomplete_historical_coverage,
        "removed native history survives durable cache reload");
    request.mode = "repair";
    write(source, summary("retained", 5));
    require(successful_page(request).records[0].total_tokens == 5, "current same-session correction may reduce totals exactly");
    const auto cache = addon::path_from_utf8(request.cache_selection->accepted->root) / "records";
    const auto accepted = read(cache);
    const auto occupied = addon::path_from_utf8(request.cache_selection->candidate_root) / "occupied";
    fs::create_directory(occupied);
    require(!invoke(request).result && read(cache) == accepted,
        "nonempty candidate allocation fails without altering accepted evidence");
    fs::remove(occupied);
    write(cache, accepted.substr(0, accepted.size() / 2));
    const auto corrupt = invoke(request);
    require(!corrupt.result, "truncated required cache fails instead of rebuilding incomplete totals");
    write(cache, accepted);

    Workspace foreign;
    auto other = request_for(foreign.path);
    const auto other_root = addon::path_from_utf8(other.context.profile_storage->root);
    const auto foreign_cache = other_root / "foreign-accounting";
    require(fs::create_directory(foreign_cache), "allocate foreign accounting fixture");
    write(foreign_cache / "records", accepted);
    addon::Contract_failure failure;
    const auto foreign_reference = addon::seal_usage_cache_candidate(qwen::path_string(foreign_cache),
        other.context.profile_storage->root, other.context.account_context, addon::random_nonce_hex(),
        "qwen-evidence-cache@1", [] { return false; }, &failure);
    require(foreign_reference.has_value(), "seal the current foreign-account cache fixture");
    other.cache_selection->accepted = foreign_reference;
    require(invoke(other).failure == "usage_cache_owner_mismatch", "cache footer cannot confer another account's ownership");
    require(addon::validate_usage_cache_candidate(*request.cache_selection->accepted,
        request.context.profile_storage->root, request.context.account_context,
        [] { return false; }, &failure).has_value(), "failed candidates leave restored accepted evidence intact");
}

void equivalent_windows_profile_spelling_reuses_cache()
{
#ifdef _WIN32
    Workspace workspace;
    const auto home = workspace.path / "CaseProfile";
    const auto equivalent = workspace.path / "caseprofile";
    auto first = request_for(home);
    write(home / "usage_record.jsonl", summary("retained", 30));
    require(successful_page(first).records[0].total_tokens == 30, "initial profile spelling persists evidence");
    fs::remove(home / "usage_record.jsonl");
    auto next = request_for(equivalent);
    require(next.context.account_context->adapter_account_key == first.context.account_context->adapter_account_key,
        "Windows casing aliases have the same admitted account identity");
    next.context.account_context = first.context.account_context;
    next.context.profile_storage = first.context.profile_storage;
    select_next_cache(next, first.cache_selection->accepted);
    require(qwen::profile_for_context(next.context).home == equivalent,
        "native profile spelling may vary while the selected account context remains exact");
    addon::Contract_failure failure;
    require(addon::validate_usage_cache_candidate(*next.cache_selection->accepted,
        next.context.profile_storage->root, next.context.account_context,
        [] { return false; }, &failure).has_value(), "native profile alias preserves the selected account cache binding");
    const auto retained = successful_page(next);
    require(retained.records.size() == 1 && retained.records[0].total_tokens == 30 && retained.incomplete_historical_coverage,
        "equivalent Windows profile spelling reloads required durable evidence across fresh scans");
    const auto other = workspace.path / "OtherProfile";
    fs::create_directories(other);
    for (const bool change_account_key : {false, true}) {
        auto foreign = next.context;
        if (change_account_key) {
            foreign.account_context->adapter_account_key = qwen::account_key(other);
        }
        else {
            foreign.account_context->profile_path = qwen::path_string(other);
        }
        bool rejected = false;
        try { (void)qwen::profile_for_context(foreign); }
        catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "account_profile_mismatch";
        }
        require(rejected, "a different native profile cannot replace the selected account");
    }
#endif
}

#ifndef _WIN32
void posix_profile_uses_qwen_path_join()
{
    Workspace workspace;
    const auto selected = workspace.path / "profile";
    const auto peer = workspace.path / "other";
    fs::create_directories(peer / "child");
    fs::create_directory_symlink(peer / "child", workspace.path / "link");
    const auto raw = workspace.path / "link/../profile";
    write(selected / "usage_record.jsonl", summary("native", 22));
    write(peer / "profile/usage_record.jsonl", summary("foreign", 199));

    auto request = request_for(selected);
    request.context.account_context->profile_path = qwen::path_string(raw);
    request.context.launch_environment->bootstrap->resolved_home = qwen::path_string(raw);
    request.context.launch_environment->effective_environment.set("QWEN_HOME", addon::Json_value(qwen::path_string(raw)));
    const auto page = successful_page(request);
    require(page.records.size() == 1 && page.records.front().record_id == "qwen:native" &&
        page.records.front().total_tokens == 22, "usage must come from the directory opened by Qwen's path.join");

    // Rebuild the old provider's sealed summary with the ambiguous raw source.
    // The envelope, footer and account binding are otherwise valid.
    auto legacy = page.records.front();
    legacy.source_path = qwen::path_string(raw / "usage_record.jsonl");
    const auto records = read(addon::path_from_utf8(page.cache_candidate->root) / "records");
    const auto body = records.substr(0, records.find('\n') + 1) + addon::serialize_json(json_util::object({
        {"summary", addon::encode_stored_usage_accounting(legacy)}})) + '\n';
    const auto footer = json_util::object({{"end", addon::Json_value(1)}, {"count", addon::Json_value(1)},
        {"sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(body)))}});
    const auto input = request.cache_selection->candidate_root;
    write(addon::path_from_utf8(input) / "records", body + addon::serialize_json(footer) + '\n');
    addon::Contract_failure failure;
    const auto sealed = addon::seal_usage_cache_candidate(input, request.context.profile_storage->root,
        request.context.account_context, "legacy", "qwen-evidence-cache@1", [] { return false; }, &failure);
    require(sealed.has_value(), "old Qwen cache fixture must seal");
    select_next_cache(request, sealed);
    for (const auto mode : {"incremental", "repair"}) {
        request.mode = mode;
        require(invoke(request).failure == "usage_cache_profile_reset_required",
            "historical raw paths must not republish another profile's evidence");
    }

    request.mode = "reset";
    select_next_cache(request, std::nullopt);
    const auto reset = successful_page(request);
    require(reset.records.size() == 1 && reset.records.front().total_tokens == 22,
        "reset rebuilds selected native history");
    request.mode = "incremental";
    request.context.launch_environment->bootstrap->resolved_home = qwen::path_string(selected);
    require(successful_page(request).records.front().total_tokens == 22,
        "the old locator still matches the same native profile after reset");
    const auto rejects_foreign = [&] {
        try { (void)qwen::profile_for_context(request.context); }
        catch (const std::runtime_error& error) {
            return std::string_view(error.what()) == "account_profile_mismatch" &&
                invoke(request).failure == "usage_source_unavailable";
        }
        return false;
    };
    request.context.account_context->profile_path = qwen::path_string(peer / "profile");
    require(rejects_foreign(), "a different native profile cannot use the retained key");
    require(qwen::account_key(selected) == qwen::account_key(selected / "") &&
        qwen::account_key(selected) == qwen::account_key(selected / ".") &&
        qwen::profile_directory("/") == fs::path("/"), "directory spellings share one account and preserve root");
    const auto old_key = "profile-" + addon::to_hex(addon::Sha256::digest(qwen::path_string(selected) + "/"));
    request.context.account_context = addon::Account_context{2, old_key, qwen::path_string(selected / ".")};
    require(qwen::profile_for_context(request.context).home == selected,
        "the original trailing-directory grant remains usable");
    request.context.account_context->profile_path = qwen::path_string(peer / "profile/");
    require(rejects_foreign(),
        "a historical trailing-directory key cannot select another profile");
}
#endif

void request_coverage_counts_errors_and_summary_requests()
{
    Workspace workspace;
    auto error = event("mixed", "\"duration_ms\":1");
    const auto type = error.find("qwen-code.api_response");
    error.replace(type, std::string("qwen-code.api_response").size(), "qwen-code.api_error");
    auto second_error = event("mixed", "\"duration_ms\":1");
    second_error.replace(second_error.find("qwen-code.api_response"),
        std::string("qwen-code.api_response").size(), "qwen-code.api_error");
    write(workspace.path / "projects/repo/chats/mixed.jsonl", error + event("mixed", counts) + second_error);
    Context context;
    auto records = qwen::scan_usage(workspace.path, context);
    require(records.size() == 1 && records[0].input.coverage->total_records == 3 &&
        records[0].input.coverage->covered_records == 1 && records[0].input.coverage->support == addon::Usage_support::UNKNOWN,
        "errors count as native requests without fabricating token observations");
    write(workspace.path / "projects/repo/chats/mixed.jsonl", error + second_error);
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].input.value == 0 && records[0].input.coverage->total_records == 2 &&
        records[0].input.coverage->covered_records == 0 && records[0].input.coverage->support == addon::Usage_support::UNKNOWN,
        "multiple errors retain known zero sum with unknown token coverage");
    write(workspace.path / "usage_record.jsonl", summary("mixed", 30));
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].input.coverage->total_records == 3 && records[0].input.coverage->covered_records == 3,
        "persisted aggregate fields cover their native models.requests count");
    auto historical = summary("mixed", 30);
    historical.erase(historical.find("\"requests\":3,"), std::string("\"requests\":3,").size());
    write(workspace.path / "usage_record.jsonl", historical);
    records = qwen::scan_usage(workspace.path, context);
    require(records[0].input.value == 20 && records[0].input.coverage->total_records == 0 &&
        records[0].input.coverage->support == addon::Usage_support::UNKNOWN,
        "old summary without request count preserves its known sum without inventing a denominator");
}

void limits_cancel_before_publication()
{
    Workspace workspace;
    const auto history = summary("one", 30);
    write(workspace.path / "usage_record.jsonl", history);
    Context context;
    qwen::Usage_limits limits;
    limits.source_bytes = history.size() - 1;
    bool rejected = false;
    try { (void)qwen::scan_usage(workspace.path, context, limits); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "source byte bound rejects rather than publishes a prefix");
    limits = {};
    limits.records = 0;
    rejected = false;
    try { (void)qwen::scan_usage(workspace.path, context, limits); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "record bound rejects before returning a candidate");
    limits = {};
    limits.result_bytes = 0;
    rejected = false;
    try { (void)qwen::scan_usage(workspace.path, context, limits); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "output byte bound rejects before returning a candidate");
    require(fs::file_size(workspace.path / "usage_record.jsonl") == history.size(), "rejected scans do not alter native durable evidence");
    context.cancelled = true;
    rejected = false;
    try { (void)qwen::scan_usage(workspace.path, context); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "cancelled scan cannot publish totals");
}

void profile_consumes_host_resolution_without_environment_mutation()
{
    Workspace workspace;
    addon::Launch_environment environment;
#ifdef _WIN32
    environment.platform_id = "windows-x86_64";
#else
    environment.platform_id = addon::host_platform_id();
#endif
    environment.effective_environment = json_util::object({{"QWEN_HOME", addon::Json_value("~\\nested\\profile")}});
    environment.bootstrap = addon::Environment_bootstrap_evidence{qwen::path_string(workspace.path / "resolved/profile")};
    const auto profile = qwen::profile_from_environment(environment);
    require(profile.home == workspace.path / "resolved/profile", "host bootstrap proof owns profile resolution even without a native home variable");
    require(qwen::environment_value(profile.environment, "QWEN_HOME") == "~\\nested\\profile", "native raw home is preserved");
    require(qwen::environment_value(profile.environment, "QWEN_RUNTIME_DIR").empty(), "unset runtime directory is not injected over native settings");
}
} // namespace

int main()
{
    try {
        native_temporal_uses_admitted_model_basis();
        temporal_bounds_preserve_original_rows_and_copy_owners();
        native_recovery_and_summary_precedence();
        partial_categories_and_torn_recovery();
        pages_bind_exact_environment_and_survive_retry();
        partial_sources_preserve_evidence_and_clean_corrections();
        forks_count_unique_calls_with_summaries_and_deleted_parents();
        copied_evidence_recovers_ancestry_without_parent_files();
        current_scans_refresh_durable_history();
        equivalent_windows_profile_spelling_reuses_cache();
#ifndef _WIN32
        posix_profile_uses_qwen_path_join();
#endif
        request_coverage_counts_errors_and_summary_requests();
        limits_cancel_before_publication();
        profile_consumes_host_resolution_without_environment_mutation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
