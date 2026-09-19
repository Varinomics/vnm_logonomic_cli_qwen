#include <logonomic_cli_addon/package_source.h>
#include "qwen_usage.h"
#include "qwen_json.h"
#include "qwen_profile.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

constexpr std::uint64_t k_max_exact = 9007199254740991;
constexpr std::size_t k_max_line_bytes = 4 * 1024 * 1024;
constexpr std::size_t k_max_page_bytes = 2 * 1024 * 1024;
constexpr std::size_t k_snapshot_bytes = 1024ULL * 1024 * 1024;

class Usage_error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct Budget
{
    Usage_limits limits;
    std::uint64_t bytes = 0;
    std::uint64_t entries = 0;
    bool incomplete = false;

    void entry()
    {
        if (entries == limits.source_entries) throw Usage_error("usage_source_capacity");
        ++entries;
    }
};

struct Session
{
    addon::Usage_record record;
    std::array<std::uint64_t, 3> values{};
    std::array<std::uint64_t, 3> covered{};
    std::uint64_t units = 0;
    bool denominator_known = true;
};

struct Event
{
    std::string model;
    std::string copied_from;
    std::string completed_date;
    std::array<std::optional<std::uint64_t>, 5> values;
    bool error = false;
};

struct Transcript
{
    addon::Usage_record metadata;
    std::map<std::string, Event> events;
    bool forked = false;
    bool published = false;
};

struct Evidence
{
    std::map<std::string, addon::Usage_record> summaries;
    std::map<std::string, Transcript> transcripts;
};

struct Snapshot
{
    std::vector<addon::Usage_record> records;
    addon::Observation_scope scope;
    std::string owner;
    std::size_t bytes = 0;
    bool incomplete = false;
};

// Serialize scans, and retain immutable snapshots rather than copying the full
// history for every page. At most two accepted snapshots plus one bounded scan
// are resident; their serialized records are each bounded by the host's 1 GiB
// candidate ceiling. Source entries and 4 MiB lines also bound object overhead.
std::timed_mutex snapshots_mutex;
std::map<std::string, std::shared_ptr<const Snapshot>> snapshots;
std::vector<std::string> snapshot_order;

void check_cancel(addon::Invocation_context& context)
{
    if (context.cancel_requested()) throw Usage_error("cancelled");
}

void add(std::uint64_t& destination, std::uint64_t value)
{
    if (value > k_max_exact - destination) throw Usage_error("usage_overflow");
    destination += value;
}

std::optional<std::uint64_t> integer(const Json& value)
{
    if (value.is_integer() && value.as_integer() >= 0 &&
        static_cast<std::uint64_t>(value.as_integer()) <= k_max_exact)
    {
        return static_cast<std::uint64_t>(value.as_integer());
    }
    if (value.is_number() && value.as_number() >= 0 && value.as_number() <= static_cast<double>(k_max_exact) &&
        std::floor(value.as_number()) == value.as_number()) return static_cast<std::uint64_t>(value.as_number());
    return std::nullopt;
}

std::string date(std::uint64_t milliseconds)
{
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds)) throw Usage_error("usage_timestamp_invalid");
#else
    if (!localtime_r(&seconds, &local)) throw Usage_error("usage_timestamp_invalid");
#endif
    char result[11]{};
    if (std::strftime(result, sizeof(result), "%Y-%m-%d", &local) != 10) throw Usage_error("usage_timestamp_invalid");
    return result;
}

std::string transcript_date(std::string_view value)
{
    // ChatRecord timestamps are emitted by Date.toISOString(). Convert to the
    // same local calendar day as durable summary timestamps, including midnight.
    if (value.size() != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z')
    {
        throw Usage_error("usage_timestamp_invalid");
    }
    const auto number = [&](std::size_t offset, std::size_t length) {
        unsigned result = 0;
        const auto parsed = std::from_chars(value.data() + offset, value.data() + offset + length, result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + offset + length) throw Usage_error("usage_timestamp_invalid");
        return result;
    };
    const std::chrono::year_month_day day{std::chrono::year(static_cast<int>(number(0, 4))),
        std::chrono::month(number(5, 2)), std::chrono::day(number(8, 2))};
    const auto hours = number(11, 2);
    const auto minutes = number(14, 2);
    const auto seconds = number(17, 2);
    if (!day.ok() || hours > 23 || minutes > 59 || seconds > 59) throw Usage_error("usage_timestamp_invalid");
    const auto instant = std::chrono::sys_days(day) + std::chrono::hours(hours) +
        std::chrono::minutes(minutes) + std::chrono::seconds(seconds) + std::chrono::milliseconds(number(20, 3));
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
    if (milliseconds < 0) throw Usage_error("usage_timestamp_invalid");
    return date(static_cast<std::uint64_t>(milliseconds));
}

bool read_line(std::istream& input, std::string* line, Budget& budget, addon::Invocation_context& context)
{
    line->clear();
    char byte = 0;
    while (input.get(byte)) {
        if (budget.bytes == budget.limits.source_bytes) throw Usage_error("usage_source_capacity");
        ++budget.bytes;
        if ((budget.bytes & 4095) == 0) check_cancel(context);
        if (byte == '\n') return true;
        if (line->size() == k_max_line_bytes) throw Usage_error("usage_record_too_large");
        line->push_back(byte);
    }
    if (input.bad()) throw Usage_error("usage_source_unavailable");
    return !line->empty();
}

template<class Consumer>
bool read_objects(const fs::path& path, Budget& budget, addon::Invocation_context& context, Consumer consume)
{
    if (!fs::is_regular_file(path)) throw Usage_error("usage_source_unavailable");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Usage_error("usage_source_unavailable");
    std::string line;
    bool complete = true;
    while (read_line(input, &line, budget, context)) {
        check_cancel(context);
        budget.entry();
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (const auto row = parse(line)) {
            if (row->is_object()) consume(*row);
            else {
                budget.incomplete = true;
                complete = false;
            }
            continue;
        }
        // Match jsonl-utils recovery of concatenated complete objects. A torn
        // trailing append does not discard complete evidence earlier in a file.
        budget.incomplete = true;
        complete = false;
        std::size_t start = 0;
        unsigned depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (std::size_t index = 0; index < line.size(); ++index) {
            const auto byte = line[index];
            if (quoted) {
                if (escaped) escaped = false;
                else if (byte == '\\') escaped = true;
                else if (byte == '"') quoted = false;
                continue;
            }
            if (byte == '"') quoted = true;
            else if (byte == '{') {
                if (depth++ == 0) start = index;
            }
            else if (byte == '}' && depth != 0 && --depth == 0) {
                if (const auto row = parse(std::string_view(line).substr(start, index - start + 1)); row && row->is_object()) consume(*row);
            }
        }
    }
    return complete;
}

void configure(Session& session, std::string_view id, std::string_view completed_date, std::string_view project,
    std::string_view source, bool transcript)
{
    if (id.empty() || id.size() > 2048 || project.size() > 32768) throw Usage_error("usage_record_invalid");
    session.record.record_id = "qwen:" + std::string(id);
    session.record.session_key = session.record.record_id;
    session.record.completed_date = std::string(completed_date);
    session.record.source_path = std::string(source);
    if (!project.empty()) session.record.repository_path = std::string(project);
    session.record.semantics = addon::Usage_counter_semantics::CUMULATIVE_FINAL;
    session.record.counter_field = transcript ? "ui_telemetry.uiEvent.total_token_count" : "usage_record.models.*.totalTokens";
    session.record.source_counter_semantics = transcript ? "per_event_sum" : "cumulative_final";
}

void model_name(Session& session, std::string_view model)
{
    if (model.size() > 4096) throw Usage_error("usage_record_invalid");
    if (!session.record.model) session.record.model = std::string(model);
    else if (*session.record.model != model) session.record.model = "multiple";
}

void finish(Session& session)
{
    const auto apply = [&](addon::Usage_amount& amount, std::size_t index) {
        amount.applicable = true;
        amount.value = session.values[index]; // Known partial sum; coverage carries the missing observations.
        amount.coverage = addon::Usage_field_coverage{session.covered[index], session.units, false,
            session.denominator_known && session.covered[index] == session.units ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN};
    };
    apply(session.record.input, 0);
    apply(session.record.output, 1);
    apply(session.record.cache_read, 2);
    session.record.cache_creation.applicable = false;
    session.record.cache_creation.coverage = addon::Usage_field_coverage{0, session.units, false, addon::Usage_support::UNSUPPORTED};
}

void append_summary(const Json& row, std::map<std::string, Session>& sessions, const fs::path& source)
{
    if (integer(member(row, "version")) != 1) return;
    const auto id = text(row, "sessionId");
    const auto timestamp = integer(member(row, "timestamp"));
    const auto& models = member(row, "models");
    if (!timestamp || !models.is_object()) throw Usage_error("usage_record_invalid");
    Session session;
    configure(session, id, date(*timestamp), text(row, "project"), path_string(source), false);
    for (const auto& entry : models.as_object()) {
        const auto total = integer(member(entry.value, "totalTokens"));
        if (!total) throw Usage_error("usage_record_invalid");
        add(session.record.total_tokens, *total);
        const auto requests = integer(member(entry.value, "requests"));
        if (requests) add(session.units, *requests);
        else session.denominator_known = false;
        const std::array names{"inputTokens", "outputTokens", "cachedTokens"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (const auto value = integer(member(entry.value, names[index]))) {
                add(session.values[index], *value);
                if (requests) add(session.covered[index], *requests);
            }
        }
        model_name(session, entry.name);
    }
    finish(session);
    sessions[id] = std::move(session); // Native durable history is last-wins.
}

struct Model
{
    std::array<std::uint64_t, 5> values{}; // prompt, candidates, cached, thoughts, total
    std::array<bool, 5> known{true, true, true, true, true};
    std::array<std::uint64_t, 3> observed{};
    std::uint64_t requests = 0;
};

bool same_event(const Event& left, const Event& right)
{
    return left.model == right.model && left.values == right.values &&
        left.error == right.error && left.completed_date == right.completed_date;
}

void append_transcript(
    const fs::path& file, Evidence& evidence, std::set<std::string>& seen,
    Budget& budget, addon::Invocation_context& context)
{
    Transcript current;
    read_objects(file, budget, context, [&](const Json& row) {
        if (current.metadata.record_id.empty()) {
            Session session;
            configure(session, text(row, "sessionId"), transcript_date(text(row, "timestamp")),
                text(row, "cwd"), path_string(file), true);
            finish(session);
            current.metadata = std::move(session.record);
        }
        if ("qwen:" + text(row, "sessionId") != current.metadata.record_id) {
            throw Usage_error("usage_session_conflict");
        }
        current.metadata.completed_date = transcript_date(text(row, "timestamp"));
        const auto& origin = member(row, "forkedFrom");
        if (!origin.is_null()) {
            current.forked = true;
            if (!bounded_string(member(origin, "sessionId"), 2048) ||
                !bounded_string(member(origin, "messageUuid"), 2048) ||
                text(origin, "messageUuid") != text(row, "uuid"))
            {
                throw Usage_error("usage_fork_provenance_invalid");
            }
        }
        if (text(row, "type") != "system" || text(row, "subtype") != "ui_telemetry") {
            return;
        }
        const auto& payload = member(member(row, "systemPayload"), "uiEvent");
        const auto type = text(payload, "event.name");
        if (type != "qwen-code.api_response" && type != "qwen-code.api_error") {
            return;
        }
        if (!bounded_string(member(row, "uuid"), 2048) ||
            !bounded_string(member(payload, "model"), 4096))
        {
            throw Usage_error("usage_record_invalid");
        }
        Event event;
        event.model          = text(payload, "model");
        event.error          = type == "qwen-code.api_error";
        event.copied_from     = origin.is_null() ? std::string{} : "qwen:" + text(origin, "sessionId");
        event.completed_date = current.metadata.completed_date;
        if (!event.error) {
            const std::array names{
                "input_token_count", "output_token_count", "cached_content_token_count",
                "thoughts_token_count", "total_token_count"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                event.values[index] = integer(member(payload, names[index]));
            }
        }
        const auto [found, inserted] = current.events.emplace(text(row, "uuid"), event);
        if (!inserted && (!same_event(found->second, event) ||
            found->second.copied_from != event.copied_from))
        {
            throw Usage_error("usage_event_conflict");
        }
    });
    if (current.metadata.record_id.empty()) {
        return;
    }
    const auto id = current.metadata.record_id;
    if (!seen.insert(id).second) {
        throw Usage_error("usage_session_conflict");
    }
    auto& retained = evidence.transcripts[id];
    current.metadata.completed_date = std::max(
        current.metadata.completed_date, retained.metadata.completed_date);
    for (const auto& [uuid, event] : retained.events) {
        (void)event;
        if (!current.events.contains(uuid)) {
            budget.incomplete = true;
        }
    }
    retained.metadata = std::move(current.metadata);
    retained.forked   = retained.forked || current.forked;
    // Absence is not a correction. A complete observed UUID can correct its
    // counters, but truncation or pruning cannot remove another accepted UUID.
    for (auto& [uuid, event] : current.events) {
        retained.events.insert_or_assign(uuid, std::move(event));
    }
}

void accumulate(Model& model, const Event& event)
{
    ++model.requests;
    if (event.error) {
        return;
    }
    for (std::size_t index = 0; index < event.values.size(); ++index) {
        if (event.values[index]) {
            add(model.values[index], *event.values[index]);
            if (index < model.observed.size()) {
                ++model.observed[index];
            }
        }
        else {
            model.known[index] = false;
        }
    }
}

addon::Usage_record aggregate(const addon::Usage_record& metadata, const std::map<std::string, Model>& models)
{
    Session session;
    session.record = metadata;
    for (const auto& [name, model] : models) {
        // metricsToUsageRecord falls back per model only when the accumulated
        // native total is zero. Thoughts are part of this fallback, not a second
        // charge added to a nonzero reported total.
        std::uint64_t total = model.values[4];
        if (!model.known[4] || total == 0) {
            if (!model.known[0] || !model.known[1] || !model.known[3]) throw Usage_error("usage_total_unavailable");
            total = model.values[0];
            add(total, model.values[1]);
            add(total, model.values[3]);
        }
        add(session.record.total_tokens, total);
        add(session.units, model.requests);
        for (std::size_t index = 0; index < session.values.size(); ++index) {
            add(session.values[index], model.values[index]);
            add(session.covered[index], model.observed[index]);
        }
        model_name(session, name);
    }
    finish(session);
    return session.record;
}

void collect(
    const fs::path& home, Evidence& evidence, Budget& budget, addon::Invocation_context& context)
{
    if (!fs::is_directory(home)) throw Usage_error("usage_profile_unavailable");
    std::map<std::string, Session> sessions;
    const auto history = home / "usage_record.jsonl";
    bool summaries_complete = true;
    if (fs::exists(history)) {
        bool understood = true;
        const bool framed = read_objects(history, budget, context, [&](const Json& row) {
            if (integer(member(row, "version")) != 1) {
                understood = false;
                budget.incomplete = true;
                return;
            }
            append_summary(row, sessions, history);
            if (sessions.size() > budget.limits.records) {
                throw Usage_error("usage_history_capacity");
            }
        });
        summaries_complete = framed && understood;
    }
    for (const auto& [id, record] : evidence.summaries) {
        (void)record;
        if (!sessions.contains(id.substr(5))) {
            budget.incomplete = true;
        }
    }
    for (auto& [id, session] : sessions) {
        (void)id;
        const auto key = session.record.record_id;
        if (summaries_complete) {
            evidence.summaries.insert_or_assign(key, std::move(session.record));
        }
        else {
            // Recovered rows are useful first evidence, but cannot supersede a
            // retained aggregate: the missing suffix may contain its final row.
            const auto transcript = evidence.transcripts.find(key);
            if (transcript == evidence.transcripts.end() || !transcript->second.published) {
                evidence.summaries.try_emplace(key, std::move(session.record));
            }
        }
    }
    std::set<std::string> seen;
    // usageHistoryService owns exactly global/projects/*/chats/*.jsonl.
    // QWEN_RUNTIME_DIR and nested debug/artifact files are not recovery inputs.
    const auto projects = home / "projects";
    if (fs::is_directory(projects)) for (const auto& project : fs::directory_iterator(projects)) {
        check_cancel(context);
        budget.entry();
        const auto chats = project.path() / "chats";
        if (!fs::is_directory(chats)) continue;
        for (const auto& entry : fs::directory_iterator(chats)) {
            check_cancel(context);
            budget.entry();
            const auto name = path_string(entry.path().filename());
            if (!name.ends_with(".jsonl") || name.ends_with(".ledger.jsonl") || !entry.is_regular_file()) continue;
            append_transcript(entry.path(), evidence, seen, budget, context);
            if (evidence.transcripts.size() > budget.limits.records) {
                throw Usage_error("usage_history_capacity");
            }
        }
    }
    for (const auto& [id, transcript] : evidence.transcripts) {
        (void)transcript;
        if (!seen.contains(id)) {
            budget.incomplete = true;
        }
    }
}

std::vector<addon::Usage_record> records_for(
    Evidence& evidence, Budget& budget, addon::Invocation_context& context)
{
    // A UUID survives every fork, while prompt_id and forkedFrom name the
    // immediate copy. Resolve that copy graph before adding any token counters.
    std::map<std::string, std::map<std::string, const Event*>> occurrences;
    std::uint64_t event_count = 0;
    for (const auto& [id, transcript] : evidence.transcripts) {
        for (const auto& [uuid, event] : transcript.events) {
            if (++event_count > budget.limits.source_entries) {
                throw Usage_error("usage_history_capacity");
            }
            occurrences[uuid].emplace(id, &event);
        }
    }
    std::map<std::string, std::map<std::string, Model>> models;
    std::map<std::string, addon::Usage_record> metadata;
    for (const auto& [id, transcript] : evidence.transcripts) {
        metadata.emplace(id, transcript.metadata);
    }
    for (const auto& [uuid, copies] : occurrences) {
        (void)uuid;
        check_cancel(context);
        std::string owner;
        const auto& observation = *copies.begin()->second;
        std::map<std::string, std::string> origins;
        for (const auto& [id, event] : copies) {
            if (!same_event(observation, *event)) {
                throw Usage_error("usage_event_conflict");
            }
            std::set<std::string> visited;
            auto origin = id;
            while (!origins.contains(origin)) {
                const auto found = copies.find(origin);
                if (found == copies.end() || found->second->copied_from.empty()) {
                    break;
                }
                if (!visited.insert(origin).second) {
                    throw Usage_error("usage_fork_provenance_invalid");
                }
                origin = found->second->copied_from;
            }
            if (const auto known = origins.find(origin); known != origins.end()) {
                origin = known->second;
            }
            for (const auto& intermediate : visited) {
                origins.emplace(intermediate, origin);
            }
            origins.emplace(id, origin);
            if (!owner.empty() && owner != origin) {
                throw Usage_error("usage_event_conflict");
            }
            owner = std::move(origin);
        }
        if (!metadata.contains(owner)) {
            auto inherited = metadata.at(copies.begin()->first);
            inherited.record_id      = owner;
            inherited.session_key    = owner;
            inherited.completed_date = observation.completed_date;
            metadata.emplace(owner, inherited);
            evidence.transcripts[owner].metadata = std::move(inherited);
            budget.incomplete = true;
        }
        if (!copies.contains(owner)) {
            auto& details = metadata.at(owner);
            details.completed_date = std::max(details.completed_date, observation.completed_date);
            evidence.transcripts[owner].metadata = details;
        }
        accumulate(models[owner][observation.model], observation);
    }
    std::map<std::string, addon::Usage_record> records;
    for (const auto& [id, details] : metadata) {
        const auto transcript = evidence.transcripts.find(id);
        if (!models.contains(id) && (transcript == evidence.transcripts.end() ||
            (transcript->second.events.empty() && !transcript->second.forked &&
                !transcript->second.published)))
        {
            continue;
        }
        records.emplace(id, aggregate(details, models[id]));
        evidence.transcripts[id].published = true;
    }
    for (const auto& [id, summary] : evidence.summaries) {
        const auto transcript = evidence.transcripts.find(id);
        if (transcript == evidence.transcripts.end() || !transcript->second.forked) {
            records.insert_or_assign(id, summary);
            continue;
        }
        std::map<std::string, Model> gross_models;
        for (const auto& [uuid, event] : transcript->second.events) {
            (void)uuid;
            accumulate(gross_models[event.model], event);
        }
        const auto gross = aggregate(transcript->second.metadata, gross_models);
        // A native fork summary includes copied telemetry without identifying
        // it. Only matching retained events establish an exact partition.
        if (gross.total_tokens != summary.total_tokens ||
            gross.input.value != summary.input.value ||
            gross.output.value != summary.output.value ||
            gross.cache_read.value != summary.cache_read.value ||
            gross.input.coverage->total_records != summary.input.coverage->total_records)
        {
            throw Usage_error("usage_fork_partition_unavailable");
        }
    }
    std::vector<addon::Usage_record> result;
    std::uint64_t bytes = 0;
    if (records.size() > budget.limits.records) {
        throw Usage_error("usage_history_capacity");
    }
    for (auto& [id, record] : records) {
        (void)id;
        check_cancel(context);
        const auto encoded = addon::encode_usage_record(record, 2);
        addon::Contract_failure failure;
        if (!addon::decode_usage_record(encoded, &failure, 2)) throw Usage_error("usage_record_invalid");
        const auto size = addon::serialize_json(encoded).size();
        if (size > k_max_page_bytes || size > budget.limits.result_bytes - bytes) throw Usage_error("usage_history_capacity");
        bytes += size;
        result.push_back(std::move(record));
    }
    return result; // map order is the stable public record-id order.
}

// Exactly three owned files: accepted records, one replacement, and an empty
// cross-generation lock. Each data file is capped at 1 GiB before writes;
// aggregate disk use cannot exceed 2 GiB. No generations or sort runs accumulate.
void regular_or_absent(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw Usage_error("usage_storage_unavailable");
    if (fs::exists(status) && (!fs::is_regular_file(status) || fs::file_size(path) > k_snapshot_bytes))
        throw Usage_error("usage_storage_invalid");
}

class Storage_lock final
{
public:
    Storage_lock(const fs::path& path, addon::Invocation_context& context)
    {
        regular_or_absent(path);
#ifdef _WIN32
        while (m_handle == INVALID_HANDLE_VALUE) {
            check_cancel(context);
            m_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_SHARING_VIOLATION) throw Usage_error("usage_storage_lock_failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#else
        m_descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (m_descriptor < 0) throw Usage_error("usage_storage_lock_failed");
        while (flock(m_descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto failure = errno;
            if (context.cancel_requested() || (failure != EWOULDBLOCK && failure != EAGAIN && failure != EINTR)) {
                close(m_descriptor);
                m_descriptor = -1;
                throw Usage_error(context.cancel_requested() ? "cancelled" : "usage_storage_lock_failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
    }
    ~Storage_lock()
    {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
        if (m_descriptor >= 0) close(m_descriptor);
#endif
    }
    Storage_lock(const Storage_lock&) = delete;
    Storage_lock& operator=(const Storage_lock&) = delete;
private:
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_descriptor = -1;
#endif
};

fs::path storage_directory(const addon::Usage_scan_request& request)
{
    if (!request.context.profile_storage) throw Usage_error("usage_profile_storage_unavailable");
    const auto root = addon::path_from_utf8(request.context.profile_storage->root);
    if (!fs::is_directory(fs::symlink_status(root))) throw Usage_error("usage_storage_invalid");
    const auto directory = root / "qwen-usage-v1";
    std::error_code error;
    if (!fs::exists(directory, error)) fs::create_directory(directory);
    if (!fs::is_directory(fs::symlink_status(directory))) throw Usage_error("usage_storage_invalid");
    return directory;
}

void inspect_storage(const fs::path& directory)
{
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (++count > 3) throw Usage_error("usage_storage_capacity");
        const auto name = path_string(entry.path().filename());
        if (name != "records" && name != "records.pending" && name != "lock") throw Usage_error("usage_storage_invalid");
        regular_or_absent(entry.path());
        if (name == "lock" && fs::file_size(entry.path()) != 0) throw Usage_error("usage_storage_invalid");
    }
}

std::string cache_owner(const addon::Usage_scan_request& request, const fs::path& home)
{
    return addon::serialize_json_canonical(object({
        {"account", Json(request.context.account_context ? request.context.account_context->adapter_account_key : std::string{})},
        {"profile", Json(account_key(home))}}));
}

void validate_seed(const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    if (!request.native_file_cache) return;
    const auto& seed = *request.native_file_cache;
    const auto account = request.context.account_context ? request.context.account_context->adapter_account_key : std::string{};
    if (seed.source_account_key != account) throw Usage_error("native_cache_account_mismatch");
    const auto source = addon::path_from_utf8(seed.path);
    const auto root = fs::canonical(addon::path_from_utf8(request.context.profile_storage->root));
    const auto resolved = fs::canonical(source);
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") throw Usage_error("native_cache_path_invalid");
    regular_or_absent(source);
    std::ifstream input(source, std::ios::binary);
    if (!input) throw Usage_error("native_cache_unavailable");
    addon::Sha256 digest;
    std::array<char, 65536> chunk{};
    std::uint64_t bytes = 0;
    while (input) {
        check_cancel(context);
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > k_snapshot_bytes - bytes) throw Usage_error("native_cache_capacity");
        bytes += count;
        digest.update(chunk.data(), count);
    }
    if (input.bad()) throw Usage_error("native_cache_unavailable");
    if (addon::to_hex(digest.finish()) != seed.sha256_hex) throw Usage_error("native_cache_digest_mismatch");
    // Qwen has no pre-add-on native FILE_CACHE owner. Never interpret another
    // provider's opaque retained wrappers as Qwen session evidence.
    if (bytes != 0) throw Usage_error("native_cache_format_unsupported");
}

Json encode_event(std::string_view session, std::string_view uuid, const Event& event)
{
    Json::Array values;
    for (const auto& value : event.values) {
        values.emplace_back(value ? Json(*value) : Json{});
    }
    return object({
        {"session", Json(std::string(session))}, {"uuid", Json(std::string(uuid))},
        {"model", Json(event.model)}, {"copied_from", Json(event.copied_from)},
        {"completed_date", Json(event.completed_date)},
        {"error", Json(event.error)}, {"values", Json(std::move(values))}});
}

template<class Consumer>
void visit_evidence(const Evidence& evidence, Consumer consume)
{
    for (const auto& [id, summary] : evidence.summaries) {
        (void)id;
        consume(object({{"summary", addon::encode_usage_record(summary, 2)}}));
    }
    for (const auto& [id, transcript] : evidence.transcripts) {
        consume(object({
            {"transcript", addon::encode_usage_record(transcript.metadata, 2)},
            {"forked", Json(transcript.forked)}, {"published", Json(transcript.published)}}));
        for (const auto& [uuid, event] : transcript.events) {
            consume(encode_event(id, uuid, event));
        }
    }
}

std::optional<Evidence> read_cache(const fs::path& path, std::string_view owner,
    addon::Invocation_context& context)
{
    regular_or_absent(path);
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Usage_error("usage_cache_unavailable");
    Budget budget{Usage_limits{}};
    std::string line;
    addon::Sha256 digest;
    if (!read_line(input, &line, budget, context)) throw Usage_error("usage_cache_invalid");
    const auto header = parse(line);
    if (!header || !exact_keys(*header, {"version", "owner"}) || integer(member(*header, "version")) != 1 || text(*header, "owner") != owner)
        throw Usage_error("usage_cache_owner_mismatch");
    digest.update(line); digest.update("\n");
    Evidence evidence;
    std::uint64_t count = 0;
    std::uint64_t events = 0;
    while (read_line(input, &line, budget, context)) {
        check_cancel(context);
        const auto row = parse(line);
        if (!row) throw Usage_error("usage_cache_invalid");
        if (row->find("end")) {
            if (!exact_keys(*row, {"end", "count", "sha256"}) || integer(member(*row, "end")) != 1 ||
                integer(member(*row, "count")) != count || text(*row, "sha256") != addon::to_hex(digest.finish()) ||
                input.peek() != std::char_traits<char>::eof()) throw Usage_error("usage_cache_invalid");
            return evidence;
        }
        ++count;
        addon::Contract_failure failure;
        if (row->find("summary") || row->find("transcript")) {
            const bool summary = row->find("summary") != nullptr;
            auto record = addon::decode_usage_record(member(*row, summary ? "summary" : "transcript"), &failure, 2);
            if (!record || !record->record_id.starts_with("qwen:") ||
                record->session_key != record->record_id)
            {
                throw Usage_error("usage_cache_invalid");
            }
            const auto id = record->record_id;
            if (summary) {
                if (!exact_keys(*row, {"summary"}) ||
                    !evidence.summaries.emplace(id, std::move(*record)).second ||
                    evidence.summaries.size() > budget.limits.records)
                {
                    throw Usage_error("usage_cache_invalid");
                }
            }
            else {
                if (!exact_keys(*row, {"transcript", "forked", "published"}) ||
                    !member(*row, "forked").is_boolean() || !member(*row, "published").is_boolean() ||
                    record->total_tokens != 0 || record->model ||
                    !evidence.transcripts.emplace(id, Transcript{
                        std::move(*record), {}, member(*row, "forked").as_boolean(),
                        member(*row, "published").as_boolean()}).second ||
                    evidence.transcripts.size() > budget.limits.records)
                {
                    throw Usage_error("usage_cache_invalid");
                }
            }
        }
        else {
            const auto transcript = evidence.transcripts.find(text(*row, "session"));
            const auto& values = member(*row, "values");
            if (!exact_keys(*row, {"session", "uuid", "model", "copied_from", "completed_date", "error", "values"}) ||
                transcript == evidence.transcripts.end() || !bounded_string(member(*row, "uuid"), 2048) ||
                !bounded_string(member(*row, "model"), 4096) ||
                !bounded_string(member(*row, "copied_from"), 2053, true) ||
                !member(*row, "error").is_boolean() || !values.is_array() || values.as_array().size() != 5 ||
                ++events > budget.limits.source_entries)
            {
                throw Usage_error("usage_cache_invalid");
            }
            Event event;
            event.model          = text(*row, "model");
            event.copied_from     = text(*row, "copied_from");
            event.completed_date = text(*row, "completed_date");
            event.error          = member(*row, "error").as_boolean();
            auto date_record = transcript->second.metadata;
            date_record.completed_date = event.completed_date;
            if (!addon::decode_usage_record(addon::encode_usage_record(date_record, 2), &failure, 2) ||
                (!event.copied_from.empty() && (!event.copied_from.starts_with("qwen:") ||
                    event.copied_from.size() == 5 || !transcript->second.forked)))
            {
                throw Usage_error("usage_cache_invalid");
            }
            for (std::size_t index = 0; index < event.values.size(); ++index) {
                const auto& value = values.as_array()[index];
                event.values[index] = integer(value);
                if ((!value.is_null() && !event.values[index]) || (event.error && !value.is_null())) {
                    throw Usage_error("usage_cache_invalid");
                }
            }
            if (!transcript->second.events.emplace(text(*row, "uuid"), std::move(event)).second) {
                throw Usage_error("usage_cache_invalid");
            }
        }
        digest.update(line); digest.update("\n");
    }
    throw Usage_error("usage_cache_incomplete");
}

void replace_cache(const fs::path& directory, std::string_view owner,
    const Evidence& evidence, addon::Invocation_context& context)
{
    const auto pending = directory / "records.pending";
    const auto accepted = directory / "records";
    regular_or_absent(pending);
    regular_or_absent(accepted);
    // Account for header, footer and line delimiters before creating/truncating
    // the replacement. Both the accepted and pending files have this same cap.
    const auto header = addon::serialize_json(object({{"version", Json(std::int64_t{1})}, {"owner", Json(std::string(owner))}}));
    std::uint64_t bytes = header.size() + 1 + 256; // bounded footer including count/digest
    std::uint64_t count = 0;
    visit_evidence(evidence, [&](const Json& row) {
        check_cancel(context);
        const auto size = addon::serialize_json(row).size() + 1;
        if (size > k_max_line_bytes) {
            throw Usage_error("usage_cache_capacity");
        }
        if (size > k_snapshot_bytes - bytes) throw Usage_error("usage_cache_capacity");
        bytes += size;
        ++count;
    });
    std::ofstream output(pending, std::ios::binary | std::ios::trunc);
    if (!output) throw Usage_error("usage_storage_write_failed");
    try {
        addon::Sha256 digest;
        const auto write = [&](std::string_view line) {
            output.write(line.data(), static_cast<std::streamsize>(line.size()));
            output.put('\n');
            if (!output) throw Usage_error("usage_storage_write_failed");
            digest.update(line); digest.update("\n");
        };
        write(header);
        visit_evidence(evidence, [&](const Json& row) {
            check_cancel(context);
            write(addon::serialize_json(row));
        });
        output << addon::serialize_json(object({{"end", Json(std::int64_t{1})},
            {"count", Json(count)}, {"sha256", Json(addon::to_hex(digest.finish()))}})) << '\n';
        output.flush();
        if (!output) throw Usage_error("usage_storage_write_failed");
        output.close();
        if (!output) throw Usage_error("usage_storage_write_failed");
        check_cancel(context);
#ifdef _WIN32
        const auto handle = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw Usage_error("usage_storage_write_failed");
        const auto synced = FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!synced) throw Usage_error("usage_storage_write_failed");
        if (!MoveFileExW(pending.c_str(), accepted.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw Usage_error("usage_storage_replace_failed");
#else
        const int descriptor = open(pending.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) throw Usage_error("usage_storage_write_failed");
        const auto synced = fsync(descriptor);
        close(descriptor);
        if (synced != 0) throw Usage_error("usage_storage_write_failed");
        fs::rename(pending, accepted);
        const int directory_descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_descriptor < 0) throw Usage_error("usage_storage_write_failed");
        const auto directory_synced = fsync(directory_descriptor);
        close(directory_descriptor);
        if (directory_synced != 0) throw Usage_error("usage_storage_write_failed");
#endif
    } catch (...) {
        output.close();
        std::error_code error;
        fs::remove(pending, error);
        throw;
    }
}

void collect_retained(const addon::Usage_scan_request& request, const fs::path& home,
    Snapshot& candidate, addon::Invocation_context& context)
{
    const auto directory = storage_directory(request);
    Storage_lock storage_lock(directory / "lock", context);
    inspect_storage(directory);
    validate_seed(request, context);
    const auto owner = cache_owner(request, home);
    auto retained = read_cache(directory / "records", owner, context);
    if (!retained && request.native_file_cache && request.native_file_cache->cache_required)
        throw Usage_error("native_cache_evidence_unavailable");
    Budget budget{Usage_limits{}};
    auto evidence = retained ? std::move(*retained) : Evidence{};
    collect(home, evidence, budget, context);
    candidate.records = records_for(evidence, budget, context);
    candidate.incomplete = budget.incomplete;
    for (const auto& record : candidate.records) {
        check_cancel(context);
        const auto size = addon::serialize_json(addon::encode_usage_record(record, 2)).size();
        if (size > k_snapshot_bytes - candidate.bytes) throw Usage_error("usage_history_capacity");
        candidate.bytes += size;
    }
    replace_cache(directory, owner, evidence, context);
}

} // namespace

std::vector<addon::Usage_record> scan_usage(const fs::path& home,
    addon::Invocation_context& context, Usage_limits limits)
{
    Budget budget{limits};
    Evidence evidence;
    collect(home, evidence, budget, context);
    return records_for(evidence, budget, context);
}

void handle_usage(addon::Invocation_context& context)
{
    if (context.request().operation != "scan") { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure, 2);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code); return; }
    try {
        if (!request->context.launch_environment || !request->context.launch_environment->bootstrap)
            throw Usage_error("environment_bootstrap_unavailable");
        const auto profile = profile_for_context(request->context);
        auto identity = *request;
        identity.snapshot_id.reset();
        identity.cursor.reset();
        const auto owner = addon::to_hex(addon::Sha256::digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(identity, 2))));
        std::size_t begin = 0;
        std::shared_ptr<const Snapshot> snapshot;
        std::string identifier;
        std::unique_lock lock(snapshots_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(100))) check_cancel(context);
        check_cancel(context);
        if (request->snapshot_id) {
            if (!request->cursor) throw Usage_error("usage_cursor_invalid");
            const auto parsed = std::from_chars(request->cursor->data(), request->cursor->data() + request->cursor->size(), begin);
            if (parsed.ec != std::errc{} || parsed.ptr != request->cursor->data() + request->cursor->size() || std::to_string(begin) != *request->cursor)
                throw Usage_error("usage_cursor_invalid");
            const auto found = snapshots.find(*request->snapshot_id);
            if (found == snapshots.end() || found->second->owner != owner || begin > found->second->records.size()) throw Usage_error("usage_snapshot_expired");
            identifier = *request->snapshot_id;
            snapshot = found->second;
        }
        else {
            if (request->cursor) throw Usage_error("usage_cursor_invalid");
            auto candidate = std::make_shared<Snapshot>();
            collect_retained(*request, profile.home, *candidate, context);
            candidate->owner = owner;
            if (request->context.account_context) candidate->scope.source_account_key = request->context.account_context->adapter_account_key;
            identifier = addon::random_nonce_hex();
            while (!snapshot_order.empty()) {
                std::size_t retained = candidate->bytes;
                for (const auto& [key, value] : snapshots) { (void)key; retained += value->bytes; }
                if (snapshot_order.size() < 2 && retained <= k_snapshot_bytes) break;
                snapshots.erase(snapshot_order.front());
                snapshot_order.erase(snapshot_order.begin());
            }
            snapshots.emplace(identifier, candidate);
            snapshot_order.push_back(identifier);
            snapshot = std::move(candidate);
        }
        addon::Usage_scan_page page;
        page.scope = snapshot->scope;
        page.snapshot_id = identifier;
        page.incomplete_historical_coverage = snapshot->incomplete;
        std::size_t end = begin;
        std::size_t bytes = 0;
        while (end < snapshot->records.size() && page.records.size() < addon::k_max_usage_page_records) {
            const auto size = addon::serialize_json(addon::encode_usage_record(snapshot->records[end], 2)).size();
            if (size > k_max_page_bytes - bytes) break;
            bytes += size;
            page.records.push_back(snapshot->records[end++]);
        }
        if (end == begin && end != snapshot->records.size()) throw Usage_error("usage_record_too_large");
        page.complete = end == snapshot->records.size();
        if (!page.complete) page.next_cursor = std::to_string(end);
        check_cancel(context);
        lock.unlock();
        context.succeed(addon::encode_usage_scan_result(page, 2));
    } catch (const Usage_error& error) {
        context.fail(error.what() == std::string_view("cancelled") ? addon::Result_status::CANCELLED : addon::Result_status::FAILED, error.what());
    } catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "usage_source_unavailable");
    }
}

} // namespace qwen
