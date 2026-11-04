// Native collection sorts and validates records before paging. Include the
// implementation to exercise the page boundary with independently malformed
// snapshots without adding a production injection interface.
#include "../src/qwen_usage.cpp"

#include <iostream>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Context final : public addon::Invocation_context
{
public:
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int) override { return false; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    bool finished() const override { return result.has_value() || !failure.empty(); }
    void succeed(addon::Json_value value) override { result = std::move(value); }
    void fail(addon::Result_status, std::string reason, std::string) override { failure = std::move(reason); }

    addon::Invoke_request invocation;
    std::optional<addon::Json_value> result;
    std::string failure;
};

class Fixture final
{
public:
    Fixture()
    :
        m_home(fs::temp_directory_path() / ("qwen-pages-" + addon::random_nonce_hex()))
    {
        const auto storage = m_home / "storage";
        const auto candidate = storage / "candidate";
        fs::create_directories(candidate);
        m_request.context.launcher = {"qwen", "direct", ""};
        m_request.context.launcher_argv = {"qwen"};
        m_request.context.working_directory = qwen::path_string(m_home);
        addon::Launch_environment environment;
        environment.platform_id = addon::host_platform_id();
        environment.effective_environment = addon::Json_value::object();
        environment.bootstrap = addon::Environment_bootstrap_evidence{qwen::path_string(m_home)};
        m_request.context.launch_environment = std::move(environment);
        m_request.context.profile_storage = addon::Profile_storage_context{1, qwen::path_string(fs::canonical(storage))};
        m_request.context.account_context = addon::Account_context{2, qwen::account_key(m_home), qwen::path_string(m_home)};
        m_request.cache_selection = addon::Usage_cache_selection{std::nullopt, qwen::path_string(candidate)};
        m_owner = addon::to_hex(addon::Sha256::digest(
            addon::serialize_json_canonical(addon::encode_usage_scan_request(m_request, 3))));
        m_request.snapshot_id = "page-boundary";
    }

    ~Fixture()
    {
        qwen::snapshots.clear();
        std::error_code error;
        fs::remove_all(m_home, error);
    }

    void select(std::vector<addon::Usage_record> records)
    {
        auto snapshot = std::make_shared<qwen::Snapshot>();
        snapshot->owner = m_owner;
        snapshot->records = std::move(records);
        snapshot->scope.source_account_key = m_request.context.account_context->adapter_account_key;
        snapshot->cache_candidate = addon::Usage_cache_reference{
            m_request.cache_selection->candidate_root, *m_request.snapshot_id, std::string(64, 'a')};
        qwen::snapshots[*m_request.snapshot_id] = std::move(snapshot);
    }

    Context invoke(std::size_t begin = 0)
    {
        m_request.cursor = std::to_string(begin);
        Context context;
        context.invocation.capability = {"usage-scan", 3};
        context.invocation.operation = "scan";
        context.invocation.payload = addon::encode_usage_scan_request(m_request, 3);
        qwen::handle_usage(context);
        return context;
    }

    addon::Usage_scan_page page(std::size_t begin = 0)
    {
        const auto context = invoke(begin);
        if (!context.result) throw std::runtime_error("page failed: " + context.failure);
        const auto parsed = addon::parse_json(addon::serialize_json(*context.result), {}, nullptr);
        require(parsed.has_value(), "returned page obeys actual JSON limits");
        const auto decoded = addon::decode_usage_scan_result(*parsed, nullptr, 3);
        require(decoded.has_value(), "returned page obeys the complete usage codec");
        return *decoded;
    }

private:
    fs::path m_home;
    addon::Usage_scan_request m_request;
    std::string m_owner;
};

addon::Usage_record record(std::string id)
{
    addon::Usage_record result;
    result.record_id = "qwen:" + id;
    result.session_key = result.record_id;
    result.completed_date = "2026-09-12";
    result.total_tokens = 1;
    result.aggregate_evidence = "partial";
    result.input = {true, 1, addon::Usage_field_coverage{1, 1, false, addon::Usage_support::SUPPORTED}};
    for (auto* amount : {&result.output, &result.cache_read, &result.cache_creation}) {
        *amount = {true, 0, addon::Usage_field_coverage{1, 1, false, addon::Usage_support::SUPPORTED}};
    }
    return result;
}

void malformed_records_preserve_the_accepted_prefix()
{
    Fixture fixture;
    const auto reject = [&](addon::Usage_record invalid) {
        fixture.select({record("a"), std::move(invalid)});
        const auto prefix = fixture.page();
        require(prefix.records.size() == 1 && prefix.records.front().record_id == "qwen:a" &&
            !prefix.complete && prefix.next_cursor == "1",
            "malformed candidate neither enters the page nor advances its cursor");
        const auto next = fixture.invoke(1);
        require(!next.result && next.failure == "usage_record_too_large",
            "an unrepresentable first candidate fails without an empty successful page");
    };

    auto invalid = record("b");
    invalid.completed_date = "2026-99-99";
    reject(std::move(invalid));
    invalid = record("b");
    invalid.capabilities.set("future-proof", addon::Json_value(std::string(addon::limits::k_max_json_string_bytes + 1, 'x')));
    reject(std::move(invalid));
    invalid = record("b");
    invalid.capabilities.set("future-proof", addon::Json_value(std::string(1, static_cast<char>(0xff))));
    reject(std::move(invalid));
    addon::Json_value nested;
    for (unsigned depth = 0; depth < addon::limits::k_max_json_depth; ++depth) {
        auto parent = addon::Json_value::array();
        parent.push_back(std::move(nested));
        nested = std::move(parent);
    }
    invalid = record("b");
    invalid.capabilities.set("future-proof", std::move(nested));
    reject(std::move(invalid));
    auto members = addon::Json_value::object();
    for (unsigned index = 0; index <= addon::limits::k_max_json_members; ++index) {
        members.set(std::to_string(index), addon::Json_value{});
    }
    invalid = record("b");
    invalid.capabilities.set("future-proof", std::move(members));
    reject(std::move(invalid));
}

void ordering_and_temporal_binding_remain_enforced()
{
    Fixture fixture;
    for (const auto* last : {"b", "c"}) {
        fixture.select({record("a"), record("c"), record(last)});
        const auto page = fixture.page();
        require(page.records.size() == 2 && page.records.back().record_id == "qwen:c" &&
            !page.complete && page.next_cursor == "2",
            "descending and duplicate IDs are rejected at the adjacent boundary");
    }
    auto timed = record("b");
    addon::Usage_temporal temporal;
    temporal.parent = {timed.record_id, timed.total_tokens, "wrong-snapshot", std::nullopt};
    temporal.unavailable_tokens = timed.total_tokens;
    temporal.coverage_reason = "missing-evidence";
    timed.temporal = std::move(temporal);
    fixture.select({record("a"), timed});
    const auto page = fixture.page();
    require(page.complete && page.records.size() == 2 && page.records.back().total_tokens == 1 &&
        !page.records.back().temporal,
        "a temporal parent that cannot bind the page is omitted without losing original accounting");
}

void complete_page_bytes_bound_each_continuation()
{
    Fixture fixture;
    std::vector<addon::Usage_record> records;
    for (const auto* id : {"a", "b", "c"}) {
        auto value = record(id);
        value.capabilities.set("future-proof", addon::Json_value(std::string(900000, 'x')));
        records.push_back(std::move(value));
    }
    fixture.select(records);
    auto page = fixture.page();
    require(page.records.size() == 2 && !page.complete && page.next_cursor == "2" &&
        addon::serialize_json(addon::encode_usage_scan_result(page, 3)).size() <= 2 * 1024 * 1024,
        "a page uses the Qwen byte limit before the record-count limit");
    page.records.push_back(records.back());
    require(addon::serialize_json(addon::encode_usage_scan_result(page, 3)).size() > 2 * 1024 * 1024,
        "the refused candidate really exceeds the complete-page byte budget");
    const auto last = fixture.page(2);
    require(last.complete && last.records.size() == 1 && last.records.front().record_id == "qwen:c",
        "byte refusal leaves the next record available to its continuation");
}

} // namespace

int main()
{
    try {
        malformed_records_preserve_the_accepted_prefix();
        ordering_and_temporal_binding_remain_enforced();
        complete_page_bytes_bound_each_continuation();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
