#include "qwen_config.h"
#include "qwen_launcher.h"
#include "qwen_launcher_approval.h"
#include "qwen_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sidecar.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json_util = logonomic::cli_addon::json;

namespace {

namespace addon = logonomic::cli_addon;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

int helper_transport_fixture(int argc, char** argv)
{
    const auto capture_path = qwen::environment_value(addon::current_process_environment(), "QWEN_TEST_HELPER_CAPTURE");
    if (capture_path.empty()) {
        return 2;
    }
    if (std::string_view(argv[1]) == "request-title-instruction") {
        std::cout << R"({"accepted":false})" << '\n';
        return 0;
    }
    if (argc < 3) {
        return 2;
    }
    if (std::string_view(argv[2]) == "acknowledge") {
        return 0;
    }
    if (std::string_view(argv[2]) != "deliver") {
        return 2;
    }
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), {}};
    const auto capture = addon::path_from_utf8(capture_path);
    const bool retry = std::filesystem::exists(capture);
    std::ofstream output(capture, std::ios::binary | std::ios::app);
    output << bytes << '\n';
    output.close();
    if (!output) {
        return 2;
    }
    if (!retry) {
        return 1;
    }
    std::cout << R"({"accepted":true,"epoch":1})" << '\n';
    return 0;
}

bool launcher_approval_contract()
{
    const auto& rules = qwen::launcher_approval_rules();
    const auto yolo = addon::Launcher_approval_action::YOLOFY;
    const auto undo = addon::Launcher_approval_action::UNYOLOFY;
    const auto changed = addon::Launcher_approval_disposition::CHANGED;
    const auto windows = addon::transform_launcher_approval({
        {"qwen", "direct", ""}, "windows-x86_64", yolo}, rules);
    bool ok = check(windows.disposition == changed && windows.script_body == "qwen --yolo=true",
        "discovered Windows Qwen launcher supports Yolofy");
    for (const auto* policy : {"-y", "--approval-mode=yolo", "--approval-mode auto-edit"}) {
        const auto result = addon::transform_launcher_approval({
            {std::string("qwen ") + policy + " --model custom", "direct", ""}, "linux-x86_64", yolo}, rules);
        ok &= check(result.disposition == changed && result.script_body == "qwen --yolo=true --model custom",
            "Qwen aliases and conflicting approval modes converge without combining them");
    }
    const auto removed = addon::transform_launcher_approval({
        {"qwen -y --approval-mode=yolo", "direct", ""}, "linux-x86_64", undo}, rules);
    ok &= check(removed.disposition == changed && removed.script_body == "qwen",
        "Unyolofy removes all recognized Qwen bypass forms");
    const std::string prompt = R"(qwen --prompt="--yolo" --approval-mode plan)";
    const auto literal = addon::transform_launcher_approval({
        {prompt, "direct", ""}, "linux-x86_64", undo}, rules);
    ok &= check(literal.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        literal.script_body == prompt, "Qwen attached prompt values and non-bypass policies survive Unyolofy");
    // Oracle: the installed Qwen Code 0.24.3 yargs parser and its native option
    // declarations: -p --yolo enables yolo, while --yolo false consumes false.
    for (const auto* script : {
            R"(qwen -p "--yolo")", R"(qwen -p "--yolo" --approval-mode plan)",
            "qwen --yolo false", "qwen -y true", "qwen --sandbox false"})
    {
        for (const auto action : {yolo, undo}) {
            const auto result = addon::transform_launcher_approval({
                {script, "direct", ""}, "linux-x86_64", action}, rules);
            const auto comparison = addon::launcher_approval_comparison_key({
                {script, "direct", ""}, "linux-x86_64"}, rules);
            ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
                result.script_body.empty() && !comparison.supported,
                "ambiguous native Qwen option operands are preserved as unsupported");
        }
    }
    for (const auto* script : {"qwen false", "qwen --approval-mode yolo false"}) {
        const auto result = addon::transform_launcher_approval({
            {script, "direct", ""}, "linux-x86_64", yolo}, rules);
        ok &= check(result.disposition == changed && result.script_body == "qwen --yolo=true false",
            "attached true preserves a Qwen positional boolean-looking prompt");
    }
    for (const auto action : {yolo, undo}) {
        const auto result = addon::transform_launcher_approval({
            {"qwen --sandbox --approval-mode yolo false", "direct", ""}, "linux-x86_64", action}, rules);
        ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "approval editing cannot expose a Qwen prompt to another boolean flag");
    }
    const auto delimited = addon::transform_launcher_approval({
        {"qwen -- false", "direct", ""}, "linux-x86_64", yolo}, rules);
    ok &= check(delimited.disposition == changed && delimited.script_body == "qwen --yolo=true -- false",
        "an existing delimiter preserves Qwen literal tail operands while enabling bypass");
    // The same native parser leaves --resume valueless before --yolo, but
    // consumes the positional session-id if approval editing removes that gap.
    for (const auto* script : {"qwen --resume --yolo session-id", "qwen -r --yolo session-id"}) {
        const auto result = addon::transform_launcher_approval({
            {script, "direct", ""}, "linux-x86_64", undo}, rules);
        ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "Unyolofy cannot turn a Qwen positional into a resume selector");
    }
    for (const auto action : {yolo, undo}) {
        const auto result = addon::transform_launcher_approval({
            {"qwen --resume --approval-mode yolo session-id", "direct", ""}, "linux-x86_64", action}, rules);
        ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "replacing Qwen approval modes preserves resume operand ownership");
    }
    const auto plan_resume = addon::transform_launcher_approval({
        {"qwen --resume --approval-mode plan session-id", "direct", ""}, "linux-x86_64", yolo}, rules);
    ok &= check(plan_resume.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
        plan_resume.script_body.empty(), "Yolofy preserves a positional after a valueless Qwen resume option");
    const std::pair<const char*, const char*> resume_cases[]{
        {"qwen --resume --yolo -- session-id", "qwen --resume -- session-id"},
        {"qwen --resume=session-id --yolo prompt", "qwen --resume=session-id prompt"},
        {"qwen --resume session-id --yolo prompt", "qwen --resume session-id prompt"},
    };
    for (const auto& [before, after] : resume_cases) {
        const auto result = addon::transform_launcher_approval({
            {before, "direct", ""}, "linux-x86_64", undo}, rules);
        ok &= check(result.disposition == changed && result.script_body == after,
            "delimiters and bound Qwen resume values keep their native operand ownership");
    }
    // Native Qwen arrays remain greedy even when attached. Explicit false is
    // omission-equivalent and retains the boundary before an implicit prompt.
    const std::pair<const char*, const char*> variadic_options[]{
        {"--allowed-tools",       "run_shell_command"},
        {"--include-directories", "src"},
        {"--exclude-tools",       "run_shell_command"},
    };
    for (const auto& [option, value] : variadic_options) {
        for (const auto& arguments : {
                std::string(option),
                std::string(option) + " " + value,
                std::string(option) + "=" + value,
                std::string(option) + " " + value + " docs",
                std::string(option) + " " + value + " " + option + " docs"})
        {
            for (const auto* policy : {"--yolo", "-y", "--approval-mode yolo", "--approval-mode plan"}) {
                const std::string script = "qwen " + arguments + " " + policy + R"( "fix the tests")";
                for (const auto* platform : {"windows-x86_64", "linux-x86_64"}) {
                    for (const auto action : {yolo, undo}) {
                        const addon::Launcher_approval_transform_request request{
                            {script, "direct", ""}, platform, action};
                        const auto result = addon::transform_launcher_approval(request, rules);
                        const auto comparison = addon::launcher_approval_comparison_key({
                            request.launcher, platform}, rules);
                        const bool retained_plan = action == undo && std::string_view(policy) == "--approval-mode plan";
                        const std::string expected = retained_plan ? script : "qwen " + arguments +
                            (action == yolo ? " --yolo=true" : " --yolo=false") + R"( "fix the tests")";
                        const auto again = addon::transform_launcher_approval({
                            {expected, "direct", ""}, platform, action}, rules);
                        const auto after_key = addon::launcher_approval_comparison_key({
                            {expected, "direct", ""}, platform}, rules);
                        const std::string message = "native variadic Qwen operands retain ownership: " +
                            script + "; replacement: " + result.script_body;
                        ok &= check(result.disposition == (retained_plan
                                    ? addon::Launcher_approval_disposition::UNCHANGED : changed) &&
                                result.script_body == expected && comparison.supported && after_key.supported &&
                                comparison.key == after_key.key &&
                                again.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
                                again.script_body == expected &&
                                request.launcher.script_body == script,
                            message.c_str());
                    }
                }
            }
        }
    }
    const std::pair<const char*, const char*> scalar_cases[]{
        {R"(qwen --model custom --yolo "fix the tests")", R"(qwen --model custom "fix the tests")"},
        {R"(qwen --prompt "fix the tests" --yolo)", R"(qwen --prompt "fix the tests")"},
        {R"(qwen --yolo -- --allowed-tools "fix the tests")", R"(qwen -- --allowed-tools "fix the tests")"},
    };
    for (const auto* platform : {"windows-x86_64", "linux-x86_64"}) {
        for (const auto& [before, after] : scalar_cases) {
            const auto result = addon::transform_launcher_approval({
                {before, "direct", ""}, platform, undo}, rules);
            const auto comparison = addon::launcher_approval_comparison_key({
                {after, "direct", ""}, platform}, rules);
            ok &= check(result.disposition == changed && result.script_body == after && comparison.supported,
                "scalar values and delimited literal Qwen arguments retain safe approval edits");
        }
    }
    const auto unknown = addon::transform_launcher_approval({
        {"qwen --approval-mode future-mode", "direct", ""}, "linux-x86_64", yolo}, rules);
    ok &= check(unknown.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Qwen unknown approval modes are not rewritten");
    const auto management = addon::transform_launcher_approval({
        {"qwen mcp list", "direct", ""}, "linux-x86_64", yolo}, rules);
    ok &= check(management.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Qwen management commands remain unchanged");
    return ok;
}

bool resume_hook_failures(addon::Host_simulator& host, const std::filesystem::path& home)
{
    std::filesystem::create_directories(home);
    addon::Resume_prepare_request request;
    request.context.launcher = {"qwen", "direct", ""};
    request.context.launcher_argv = {"qwen"};
    request.context.working_directory = addon::path_to_utf8(home);
    addon::Launch_environment environment;
    environment.platform_id = addon::host_platform_id();
    environment.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(home)};
    request.context.launch_environment = environment;
    request.resume_record.set("provider_session_id", qwen::Json("019933a1-6e43-7102-a520-b3c8294cc763"));
    const auto expect = [&](addon::Host_status status, const char* reason) {
        const auto id = host.invoke({"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(request, 2), 15000);
        const auto outcome = host.wait_outcome(id, 18000);
        const std::string_view message = status == addon::Host_status::FAILED
            ? "Qwen hooks are disabled." : "";
        if (!outcome || outcome->status != status || outcome->reason_code != reason ||
            outcome->message != message)
        {
            std::cerr << "expected resume reason " << reason << ", got "
                << (outcome ? outcome->reason_code : "no outcome") << '\n';
            return false;
        }
        return true;
    };

    const auto settings = home / "settings.json";
    const auto write_settings = [
            &settings
        ](
            std::string_view content)
        {
            std::ofstream output(settings, std::ios::binary);
            output << content;
            output.close();
            if (!output) {
                throw std::runtime_error("resume settings fixture could not be written");
            }
        };
    bool ok = expect(addon::Host_status::UNAVAILABLE, "managed_hooks_unavailable");
    // A directory at the settings path cannot be read as a regular file, even as root.
    std::filesystem::create_directory(settings);
    ok &= expect(addon::Host_status::UNAVAILABLE, "managed_hooks_unavailable");
    std::filesystem::remove(settings);
    for (const auto* content : {R"({"private":"private-sentinel"})",
            R"({"disableAllHooks":true,"private":"private-sentinel")"})
    {
        write_settings(content);
        ok &= expect(addon::Host_status::UNAVAILABLE, "managed_hooks_unavailable");
    }
    write_settings(R"({"disableAllHooks":true,"private":"private-sentinel"})");
    ok &= expect(addon::Host_status::FAILED, "hooks_disabled_by_user");
    auto managed = qwen::parse_settings(qwen::updated_config(R"({"private":"private-sentinel"})"));
    managed.set("disableAllHooks", qwen::Json(true));
    write_settings(addon::serialize_json_canonical(managed));
    ok &= expect(addon::Host_status::FAILED, "hooks_disabled_by_user");
    return ok;
}

bool packaged_settings_and_approval(const std::filesystem::path& package, const std::filesystem::path& root)
{
    addon::Simulator_options options;
    options.package_directory = package;
    options.scratch_directory = root / "approval_scratch";
    options.profile_storage_root = root / "approval_profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    addon::Contract_failure failure;
    if (!check(host.start(&failure), "approval package starts")) return false;
    const auto transform_id = host.invoke({"managed-configuration", 2}, "transform",
        addon::encode_managed_configuration_transform_request({addon::random_nonce_hex(), true,
            "{\"disableAllHooks\":true,\"private\":\"private-sentinel\"}"}), 15000);
    const auto refused = host.wait_outcome(transform_id, 18000);
    if (!check(refused && refused->status == addon::Host_status::FAILED &&
            refused->reason_code == "hooks_disabled_by_user" &&
            refused->message.find("private-sentinel") == std::string::npos,
            "disabled hooks retain their safe reason without exposing settings")) return false;
    if (!resume_hook_failures(host, root / "resume-profile")) return false;
    const auto invoke = [&](const char* operation, addon::Json_value payload) {
        const auto id = host.invoke({addon::k_launcher_approval_capability_id, addon::k_launcher_approval_version},
            operation, std::move(payload), 15000);
        const auto outcome = host.wait_outcome(id, 18000);
        if (!outcome || outcome->status != addon::Host_status::SUCCESS) {
            throw std::runtime_error("packaged approval operation failed");
        }
        return outcome->payload;
    };
    const auto transform = [&](const addon::Launcher_request& launcher, addon::Launcher_approval_action action) {
        const auto result = addon::decode_launcher_approval_transform_result(invoke(
            addon::k_launcher_approval_transform_operation,
            addon::encode_launcher_approval_transform_request({launcher, "windows-x86_64", action})), &failure);
        if (!result) throw std::runtime_error("packaged approval result did not decode");
        return *result;
    };
    const auto key = [&](const addon::Launcher_request& launcher) {
        const auto result = addon::decode_launcher_approval_comparison_result(invoke(
            addon::k_launcher_approval_comparison_operation,
            addon::encode_launcher_approval_comparison_request({launcher, "windows-x86_64"})), &failure);
        if (!result || !result->supported) throw std::runtime_error("packaged comparison was unsupported");
        return result->key;
    };
    std::vector<std::string> scripts;
    for (const auto* option : {"--allowed-tools", "--include-directories", "--exclude-tools"}) {
        for (const auto& arguments : {
                std::string(option), std::string(option) + " read_file", std::string(option) + " read_file docs",
                std::string(option) + "=read_file", std::string(option) + " read_file " + option + "=docs",
                std::string(option) + "=", std::string(option) + " \"read_file, docs\""})
        {
            scripts.push_back("qwen " + arguments + R"( --yolo "fix the tests")");
        }
    }
    for (const auto* script : {
            R"(qwen --allowed-tools read_file --approval-mode yolo "fix the tests")",
            R"(qwen --allowed-tools read_file --approval-mode plan "fix the tests")",
            R"(qwen --allowed-tools read_file --yolo --prompt "fix the tests")",
            R"(qwen --allowed-tools read_file --yolo --model custom "fix the tests")",
            R"(qwen --allowed-tools read_file --yolo -- "literal tail")",
            "qwen --allowed-tools read_file --yolo", "qwen --allowed-tools read_file", "qwen --allowed-tools",
            "qwen false", "qwen true", "qwen --allowed-tools read_file --approval-mode yolo false",
            "qwen --allowed-tools read_file --yolo=false false", "qwen --yolo=true false"})
    {
        scripts.emplace_back(script);
    }
    bool ok = true;
    for (const auto& script : scripts) {
        for (const auto action : {addon::Launcher_approval_action::YOLOFY, addon::Launcher_approval_action::UNYOLOFY}) {
            const addon::Launcher_request original{script, "direct", "QWEN_APPROVAL_FIXTURE=retained"};
            const auto result = transform(original, action);
            if (!check(result.disposition != addon::Launcher_approval_disposition::UNSUPPORTED,
                    "ordinary native array/boolean forms support packaged approval edits")) return false;
            auto edited = original;
            edited.script_body = result.script_body;
            const auto repeated = transform(edited, action);
            ok &= check(repeated.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
                    repeated.script_body == result.script_body && key(original) == key(edited) &&
                    original.script_body == script,
                "packaged edits are idempotent and preserve saved-launcher comparison identity");
            auto record = addon::Json_value::object();
            record.set("action", addon::Json_value(action == addon::Launcher_approval_action::YOLOFY ? "yolofy" : "unyolofy"));
            record.set("before", addon::Json_value(script));
            record.set("after", addon::Json_value(result.script_body));
            for (const auto& [name, text] : {std::pair{"before_argv", script}, {"after_argv", result.script_body}}) {
                auto arguments = addon::Json_value::array();
                for (const auto& argument : addon::literal_argv(text)) arguments.as_array().emplace_back(argument);
                record.set(name, std::move(arguments));
            }
            std::cout << "QWEN_APPROVAL_NATIVE:" << addon::serialize_json(record) << '\n';
        }
    }
    ok &= check(key({"qwen --allowed-tools read_file --yolo prompt", "direct", ""}) !=
            key({"qwen --allowed-tools read_file prompt", "direct", ""}),
        "comparison distinguishes an implicit prompt from the same text consumed by an array");
    int exit_code = -1;
    ok &= check(host.shutdown(3000, &exit_code) && exit_code == 0, "approval package shuts down cleanly");
    return ok;
}

bool settings_contract()
{
    // Qwen settings.ts accepts JSONC; hooks/types.ts defines named commands
    // inside event arrays. The owner's other values must survive installation.
    const std::string original = R"({
  // Keep my chosen provider and credentials reference.
  "security": {"auth": {"selectedType": "openai"}},
  "hooks": {"Stop": [/* before */ {"hooks": [{"type":"command","name":"mine","command":"echo done"}]}, /* after */]},
  "custom": "https://example.invalid/*literal*/", // trailing comment
})";
    const auto configured = qwen::updated_config(original);
    const auto parsed = qwen::parse_settings(configured);
    bool ok = check(configured.find("// Keep my chosen provider") != std::string::npos,
        "unrelated settings comments survive");
    ok &= check(json_util::text(json_util::member(json_util::member(parsed, "security"), "auth"), "selectedType") == "openai",
        "selected native authentication survives");
    const auto& stop = json_util::member(json_util::member(parsed, "hooks"), "Stop");
    ok &= check(stop.is_array() && stop.as_array().size() == 2,
        "existing Stop command and managed command coexist");
    ok &= check(qwen::updated_config(configured) == configured, "hook installation is idempotent");
    ok &= check(configured.find("/* before */") != std::string::npos &&
        configured.find("/* after */") != std::string::npos, "hook-array comments survive");
    auto modified = parsed;
    auto hooks = json_util::member(modified, "hooks");
    auto modified_stop = json_util::member(hooks, "Stop");
    modified_stop.as_array().back().set("matcher", qwen::Json("custom"));
    hooks.set("Stop", modified_stop);
    modified.set("hooks", hooks);
    bool modified_refused = false;
    try {
        (void)qwen::updated_config(addon::serialize_json_canonical(modified));
    }
    catch (const std::runtime_error&) {
        modified_refused = true;
    }
    ok &= check(modified_refused, "same-name modified wrapper is not silently replaced");
    bool refused = false;
    try {
        (void)qwen::updated_config("{\"disableAllHooks\":true}");
    }
    catch (const std::runtime_error&) {
        refused = true;
    }
    ok &= check(refused, "an explicit native hook disable is respected");
    return ok;
}

bool lifecycle_contract()
{
    // hookEventHandler creates these fields directly from Config/prompt context.
    addon::Classify_event_request request;
    request.event.name = "SessionStart";
    request.event.body = json_util::object({{"session_id", qwen::Json("12345678-1234-4234-8234-123456789abc")},
        {"event_id", qwen::Json("abcdef0123456789abcdef0123456789")}, {"source", qwen::Json("startup")}});
    auto result = qwen::classify_event(request);
    bool ok = check(result.classification == addon::Classification::TRANSITION && result.observations.size() == 1,
        "native SessionStart binds the exact vendor session");
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{addon::Transition_kind::START,
        "12345678-1234-4234-8234-123456789abc", "0123456789abcdef0123456789abcdef"};
    request.event.body.set("source", qwen::Json("resume"));
    ok &= check(qwen::classify_event(request).classification == addon::Classification::IGNORED,
        "resuming the currently bound native UUID does not open another epoch");
    request.event.body.set("session_id", qwen::Json("87654321-1234-4234-8234-123456789abc"));
    result = qwen::classify_event(request);
    ok &= check(result.classification == addon::Classification::TRANSITION &&
        result.transition == addon::Transition_kind::RESUME,
        "resuming another native UUID opens its own resume epoch");
    request.event.body.set("session_id", qwen::Json("12345678-1234-4234-8234-123456789abc"));
    for (const auto* source : {"compact"}) {
        request.event.body.set("source", qwen::Json(source));
        ok &= check(qwen::classify_event(request).classification == addon::Classification::IGNORED,
            "same-session native maintenance does not open a fresh host epoch");
    }
    request.event.body.set("source", qwen::Json("branch"));
    request.event.body.set("session_id", qwen::Json("87654321-1234-4234-8234-123456789abc"));
    result = qwen::classify_event(request);
    ok &= check(result.classification == addon::Classification::TRANSITION &&
        result.transition == addon::Transition_kind::START,
        "native branch assigns a new session UUID and opens a new epoch");
    request.event.body.set("session_id", qwen::Json("12345678-1234-4234-8234-123456789abc"));
    request.event.name = "SessionEnd";
    request.event.body.set("reason", qwen::Json("clear"));
    result = qwen::classify_event(request);
    ok &= check(result.observations.size() == 1 &&
        result.observations.front().session_transition == addon::Session_transition::ENDED,
        "clear ends its old exact native session before identity rotation");
    request.event.name = "SessionStart";
    request.event.body.set("source", qwen::Json("clear"));
    request.event.body.set("session_id", qwen::Json("87654321-1234-4234-8234-123456789abc"));
    result = qwen::classify_event(request);
    ok &= check(result.classification == addon::Classification::TRANSITION &&
        result.transition == addon::Transition_kind::START,
        "clearCommand rotates Config to a new UUID before resetChat emits SessionStart");
    request.ledger.current_transition->provider_session_id = "87654321-1234-4234-8234-123456789abc";
    request.event.name = "SessionEnd";
    request.event.body.set("session_id", qwen::Json("12345678-1234-4234-8234-123456789abc"));
    ok &= check(qwen::classify_event(request).classification == addon::Classification::IGNORED,
        "a nonawaited old-ID clear end arriving after new start cannot end the new epoch");
    request.ledger.current_transition->provider_session_id = "12345678-1234-4234-8234-123456789abc";
    request.event.name = "Stop";
    request.event.body.set("prompt_id", qwen::Json("12345678-1234-4234-8234-123456789abc########7"));
    result = qwen::classify_event(request);
    ok &= check(result.observations.size() == 1 &&
        result.observations.front().phase_evidence == addon::Evidence::INFERRED &&
        result.observations.front().outcome_transition == addon::Outcome_transition::NONE,
        "a blockable native Stop does not prove successful completion");
    request.event.name = "StopFailure";
    result = qwen::classify_event(request);
    ok &= check(result.observations.size() == 1 &&
        result.observations.front().outcome_transition == addon::Outcome_transition::FAILED &&
        result.observations.front().outcome_evidence == addon::Evidence::CONFIRMED &&
        result.observations.front().provider_turn_id == "12345678-1234-4234-8234-123456789abc########7",
        "native non-blockable StopFailure proves the identified turn failed");
    request.event.name = "Stop";
    request.event.body.set("agent_id", qwen::Json("worker@team"));
    result = qwen::classify_event(request);
    ok &= check(result.classification == addon::Classification::IGNORED,
        "a child Stop never changes the root phase");
    request.event.body.set("agent_id", qwen::Json(""));
    request.event.body.set("session_id", qwen::Json("different-session"));
    ok &= check(qwen::classify_event(request).classification == addon::Classification::IGNORED,
        "stale sessions do not enter the current ledger");
    return ok;
}

bool declared_profile_admits_every_shape(const std::filesystem::path& package)
{
    // Oracle: the host's feature gate. It rejects an observation carrying a
    // fact the package's own declaration leaves unsupported, phase and all.
    const addon::Directory_package_source source(package);
    const auto bytes = source.read(
        addon::sidecar_relative_path(addon::k_lifecycle_events_capability_id, addon::k_lifecycle_events_version),
        addon::limits::k_max_sidecar_bytes,
        nullptr);
    const auto sidecar = bytes ? addon::parse_sidecar(*bytes, nullptr) : std::nullopt;
    const auto declaration = sidecar ? addon::decode_lifecycle_declaration(sidecar->payload, nullptr) : std::nullopt;
    if (!check(declaration.has_value(), "package lifecycle declaration is readable")) return false;
    addon::Attention_classification_request request;
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "native-session", std::string(32, 'b')};
    bool ok = true;
    const auto classify = [
            &request,
            &declaration,
            &ok
        ](
            const char* name,
            const char* member,
            const char* value,
            const char* shape)
        {
            request.base.event.name = name;
            request.base.event.body = json_util::object({
                {"session_id", qwen::Json("native-session")}, {"event_id", qwen::Json("helper-delivery")},
                {"prompt_id", qwen::Json("native-turn")}, {member, qwen::Json(value)}});
            const auto result = qwen::classify_attention(request);
            ok &= check(!result.base.observations.empty(), shape);
            for (const auto& observation : result.base.observations) {
                ok &= check(addon::lifecycle_profile_admits(declaration->features, observation), shape);
            }
        };
    request.base.ledger.current_transition->provider_session_id = "previous-session";
    for (const auto* start_source : {"startup", "resume", "branch", "clear"}) {
        classify("SessionStart", "source", start_source, "the declaration refuses a session start or resume");
    }
    request.base.ledger.current_transition->provider_session_id = "native-session";
    classify("UserPromptSubmit", "prompt", "Continue", "the declaration refuses a turn start");
    classify("Stop", "stop_hook_active", "false", "the declaration refuses a turn completion");
    classify("StopFailure", "error", "rate_limit", "the declaration refuses a failed turn");
    for (const auto* notification : {"permission_prompt", "elicitation_dialog"}) {
        classify("Notification", "notification_type", notification,
            "the declaration refuses an approval request or a structured question");
    }
    classify("SessionEnd", "reason", "clear", "the declaration refuses a session end");
    return ok;
}

bool attention_contract(const std::filesystem::path& package, const std::filesystem::path& root)
{
    addon::Simulator_options options;
    options.package_directory = package;
    options.scratch_directory = root / "scratch";
    options.profile_storage_root = root / "profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    addon::Contract_failure failure;
    if (!check(host.start(&failure), "actual packaged Qwen adapter starts")) return false;
    const auto invoke = [&](const addon::Capability_pair& capability, addon::Json_value payload) {
        const auto id = host.invoke(capability, addon::k_classify_event_operation, std::move(payload), 15000);
        const auto outcome = host.wait_outcome(id, 18000);
        if (!outcome) throw std::runtime_error("packaged classification did not settle");
        return *outcome;
    };
    addon::Attention_classification_request request;
    request.base.transition_nonce = std::string(32, 'a');
    request.base.ledger.current_epoch = 1;
    request.base.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "native-session", std::string(32, 'b')};
    request.base.event.name = "Notification";
    request.base.event.body = json_util::object({
        {"session_id", qwen::Json("native-session")}, {"event_id", qwen::Json("helper-delivery")},
        {"prompt_id", qwen::Json("native-turn")}, {"message", qwen::Json("private-notification-canary?")}});
    bool ok = true;
    for (const auto* notification : {"permission_prompt", "elicitation_dialog"}) {
        request.base.event.body.set("notification_type", qwen::Json(notification));
        const auto expected = qwen::classify_attention(request);
        const auto result = invoke({addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
            addon::encode_attention_classification_request(request));
        const auto decoded = addon::decode_attention_classification_result(result.payload, &failure, 2);
        if (!check(result.status == addon::Host_status::SUCCESS && decoded && decoded->attention_kind &&
                decoded->base.observations.size() == 1, "packaged notification kind decodes")) return false;
        const auto& observation = decoded->base.observations.front();
        ok &= check(decoded->attention_kind->kind ==
                (std::string(notification) == "permission_prompt" ? "permission" : "interactive_question") &&
                decoded->attention_kind->observation_index == 0,
            "native notification kinds remain distinct");
        ok &= check(observation.attention_transition == addon::Attention_transition::REQUESTED &&
                observation.attention_evidence == addon::Evidence::INFERRED && observation.attention_request_id.empty(),
            "notification is inferred and unpaired, never a fabricated request identity");
        ok &= check(observation.provider_event_name == "Notification" && observation.provider_event_id == "helper-delivery" &&
                observation.provider_session_id == "native-session" && observation.provider_turn_id == "native-turn",
            "native session/turn and helper delivery correlation survive classification");
        const auto encoded = addon::serialize_json_canonical(result.payload);
        ok &= check(encoded == addon::serialize_json_canonical(addon::encode_attention_classification_result(expected)) &&
                encoded.find("private-notification-canary") == std::string::npos,
            "packaged and local classification agree without forwarding notification prose");
        const auto legacy = invoke({"helper-ingress", 2}, addon::encode_classify_event_request(request.base));
        ok &= check(legacy.status == addon::Host_status::SUCCESS &&
                addon::serialize_json_canonical(legacy.payload) ==
                    addon::serialize_json_canonical(addon::encode_classify_event_result(decoded->base)),
            "legacy helper ingress remains exactly the companion's unchanged base");
    }
    const auto no_kind = [&](const addon::Attention_classification_request& input) {
        const auto result = invoke({addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
            addon::encode_attention_classification_request(input));
        const auto decoded = addon::decode_attention_classification_result(result.payload, &failure, 2);
        return check(result.status == addon::Host_status::SUCCESS && decoded && !decoded->attention_kind &&
                addon::serialize_json_canonical(addon::encode_classify_event_result(decoded->base)) ==
                    addon::serialize_json_canonical(addon::encode_classify_event_result(qwen::classify_event(input.base))),
            "unsupported or unavailable notification preserves base semantics without inventing a kind");
    };
    auto other = request;
    other.base.event.body.set("notification_type", qwen::Json("future_notification"));
    ok &= no_kind(other);
    other.base.event.body.set("notification_type", qwen::Json(17));
    ok &= no_kind(other);
    other = request;
    other.base.event.body.set("agent_id", qwen::Json("native-child"));
    ok &= no_kind(other);
    other = request;
    other.base.event.body.set("session_id", qwen::Json("stale-session"));
    ok &= no_kind(other);
    other = request;
    other.base.event.body.erase("event_id");
    ok &= no_kind(other);
    for (const auto* name : {"UserPromptSubmit", "Stop", "StopFailure", "SessionEnd"}) {
        other = request;
        other.base.event.name = name;
        other.pending_conversational_request_id = "host-pending-hint";
        ok &= no_kind(other);
        ok &= check(qwen::classify_attention(other).base.observations.front().attention_transition ==
                addon::Attention_transition::NONE,
            "boundary, error and Stop neither invent an answer nor classify prose as a hint");
    }
    auto malformed = addon::encode_attention_classification_request(request);
    malformed.set("unknown_envelope_member", qwen::Json(true));
    ok &= check(invoke({addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
            std::move(malformed)).status != addon::Host_status::SUCCESS,
        "companion keeps the strict request envelope");
    int exit_code = -1;
    ok &= check(host.shutdown(3000, &exit_code) && exit_code == 0, "packaged adapter shuts down cleanly");
    return ok;
}

bool context_contract(const std::filesystem::path& package, const std::filesystem::path& root)
{
    addon::Simulator_options options;
    options.package_directory    = package;
    options.scratch_directory    = root / "context_scratch";
    options.profile_storage_root = root / "context_profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    addon::Contract_failure failure;
    if (!check(host.start(&failure), "context package starts")) {
        return false;
    }
    const auto live = host.live_capabilities();
    bool ok = check(std::find(live.begin(), live.end(), addon::Capability_pair{
            addon::k_context_state_capability_id, addon::k_context_state_capability_version}) != live.end(),
        "the packaged declaration and adapter handshake negotiate context-state@2");
    ok &= check(std::find(live.begin(), live.end(), addon::Capability_pair{
            addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version}) != live.end(),
        "the packaged declaration and adapter handshake negotiate interactive-initial-prompt@1");
    const auto invoke = [
            &host
        ](
            const addon::Json_value& payload)
        {
            const auto id = host.invoke({"helper-ingress", 2}, addon::k_classify_event_operation, payload, 15000);
            const auto outcome = host.wait_outcome(id, 18000);
            addon::Contract_failure failure;
            const auto result = outcome && outcome->status == addon::Host_status::SUCCESS
                ? addon::decode_classify_event_result(outcome->payload, &failure, 2) : std::nullopt;
            if (!result) {
                throw std::runtime_error("packaged context classification did not decode");
            }
            return *result;
        };
    addon::Classify_event_request request;
    request.transition_nonce = std::string(32, 'a');
    request.ledger.current_epoch = 1;
    request.ledger.current_transition = addon::Ledger_transition{
        addon::Transition_kind::START, "12345678-1234-4234-8234-123456789abc", std::string(32, 'b')};
    request.event.name = "Stop";
    // Native buildContextUsage uses the latest prompt count and current model
    // capacity. The hook can still block Stop, so it supplies no success fact.
    request.event.body = *addon::parse_json(R"({
        "session_id":"12345678-1234-4234-8234-123456789abc",
        "prompt_id":"12345678-1234-4234-8234-123456789abc########7",
        "event_id":"stop-delivery",
        "input_tokens":32000,
        "context_limit":128000
    })", {}, nullptr);
    auto result = invoke(addon::encode_classify_event_request(request));
    ok &= check(result.context_updates.empty() && !result.context_failure && result.observations.size() == 1,
        "a host without context opt-in retains the ordinary lifecycle result");
    request.context_state_version = addon::k_context_state_capability_version;
    result = invoke(addon::encode_classify_event_request(request));
    if (!check(result.context_updates.size() == 1 && !result.context_failure,
            "native Stop yields one occupancy observation"))
    {
        return false;
    }
    const auto measured = result.context_updates.front();
    ok &= check(measured.used_tokens == 32000 && measured.context_window == 128000 &&
            !measured.compacted && !measured.compaction_count && !measured.compaction_count_revision &&
            result.observations.size() == 1 &&
            result.observations.front().outcome_transition == addon::Outcome_transition::NONE,
        "Stop occupancy is independent of cumulative billing, compaction and turn success");
    addon::Context_state state;
    addon::apply_context_update(state, measured);
    ok &= check(state.used_fraction == 0.25 && !state.compaction_count_complete,
        "native occupancy applies without inventing complete compaction history");
    auto unsupported = addon::encode_classify_event_request(request);
    unsupported.set("context_state_version", qwen::Json("unsupported"));
    result = invoke(unsupported);
    ok &= check(result.context_updates.empty() && result.observations.size() == 1 && result.context_failure &&
            result.context_failure->provider_session_id == measured.provider_session_id &&
            result.context_failure->failure.reason_code == addon::reason::k_unsupported_version,
        "an unsupported optional request retains its native source and valid lifecycle evidence");
    auto notification = request;
    notification.event.name = "Notification";
    notification.event.body.set("notification_type", qwen::Json("auth_success"));
    unsupported = addon::encode_classify_event_request(notification);
    unsupported.set("context_state_version", qwen::Json("unsupported"));
    result = invoke(unsupported);
    ok &= check(result.classification == addon::Classification::IGNORED && result.observations.empty() &&
            result.context_updates.empty() && result.context_failure &&
            result.context_failure->provider_session_id == measured.provider_session_id &&
            result.context_failure->failure.reason_code == addon::reason::k_unsupported_version,
        "ignored native auth_success notifications retain an independent optional context error");
    auto invalid_lifecycle = request;
    invalid_lifecycle.event.name = "SessionEnd";
    invalid_lifecycle.event.body.set("reason",
        qwen::Json(std::string(addon::limits::k_max_lifecycle_label_bytes + 1, 'x')));
    unsupported = addon::encode_classify_event_request(invalid_lifecycle);
    unsupported.set("context_state_version", qwen::Json("unsupported"));
    result = invoke(unsupported);
    ok &= check(result.classification == addon::Classification::IGNORED && result.observations.empty() &&
            result.lifecycle_degradation && result.context_failure &&
            result.context_failure->provider_session_id == measured.provider_session_id &&
            result.context_failure->failure.reason_code == addon::reason::k_unsupported_version,
        "lifecycle validation loss does not erase the independent context error");
    auto retired = request;
    retired.event.body.set("session_id", qwen::Json("87654321-1234-4234-8234-123456789abc"));
    unsupported = addon::encode_classify_event_request(retired);
    unsupported.set("context_state_version", qwen::Json("unsupported"));
    result = invoke(unsupported);
    ok &= check(result.context_updates.empty() && result.observations.empty() && result.context_failure &&
            result.context_failure->provider_session_id == "87654321-1234-4234-8234-123456789abc",
        "a delayed request error retains the retired native source instead of borrowing the active ledger");
    retired.event.body.erase("session_id");
    unsupported = addon::encode_classify_event_request(retired);
    unsupported.set("context_state_version", qwen::Json("unsupported"));
    result = invoke(unsupported);
    ok &= check(result.context_updates.empty() && result.context_failure &&
            result.context_failure->provider_session_id.empty() &&
            result.context_failure->failure.reason_code == addon::reason::k_unsupported_version,
        "request errors without a native source cannot withdraw the active session's context");
    for (const auto& invalid : {qwen::Json(-1), qwen::Json(0.5), qwen::Json("32000"),
             qwen::Json(9007199254740992.0), qwen::Json()})
    {
        auto missing = request;
        missing.event.body.set("input_tokens", invalid);
        result = invoke(addon::encode_classify_event_request(missing));
        ok &= check(result.context_updates.size() == 1 && result.context_updates.front().usage_unavailable &&
                !result.context_updates.front().used_tokens && !result.context_updates.front().context_window &&
                result.observations.size() == 1,
            "malformed native occupancy clears its measurement without suppressing lifecycle evidence");
    }
    auto compact = request;
    compact.event.name = "SessionStart";
    compact.event.body = json_util::object({
        {"session_id", qwen::Json(measured.provider_session_id)},
        {"source", qwen::Json("compact")}, {"event_id", qwen::Json("compact-delivery")}});
    result = invoke(addon::encode_classify_event_request(compact));
    if (!check(result.classification == addon::Classification::IGNORED && result.observations.empty() &&
            result.context_updates.size() == 1, "successful compact is context-only, not a new session"))
    {
        return false;
    }
    const auto compacted = result.context_updates.front();
    ok &= check(compacted.compacted && !compacted.compaction_count && !compacted.compaction_count_revision,
        "completed native compaction uses its event authority without a fabricated snapshot order");
    addon::apply_context_update(state, compacted);
    ok &= check(state.compaction_count == 1 && !state.compaction_count_complete && !state.used_fraction,
        "completed compaction clears occupancy and leaves unobserved resumed history unknown");
    addon::apply_context_update(state, measured);
    const auto before_replay = state;
    addon::apply_context_update(state, compacted);
    ok &= check(state == before_replay, "redelivery after a later usage update neither recounts nor clears usage");
    compact.event.body.set("event_id", qwen::Json("second-compact-delivery"));
    result = invoke(addon::encode_classify_event_request(compact));
    addon::apply_context_update(state, result.context_updates.front());
    ok &= check(state.compaction_count == 2 && !state.compaction_count_complete,
        "a distinct completed compaction increments once without claiming a complete baseline");
    for (const auto* name : {"PreCompact", "StopFailure"}) {
        auto attempt = request;
        attempt.event.name = name;
        ok &= check(invoke(addon::encode_classify_event_request(attempt)).context_updates.empty(),
            "attempts and failures are not successful compactions or occupancy observations");
    }
    for (const auto& event : {request, compact}) {
        auto other = event;
        other.event.body.set("agent_id", qwen::Json("worker@team"));
        ok &= check(invoke(addon::encode_classify_event_request(other)).context_updates.empty(),
            "child telemetry cannot change the root context");
        other = event;
        other.event.body.set("session_id", qwen::Json("87654321-1234-4234-8234-123456789abc"));
        ok &= check(invoke(addon::encode_classify_event_request(other)).context_updates.empty(),
            "stale-session telemetry cannot change the active context");
    }
    auto next = request;
    next.ledger.current_transition->provider_session_id = "87654321-1234-4234-8234-123456789abc";
    next.event.body.set("session_id", qwen::Json(next.ledger.current_transition->provider_session_id));
    result = invoke(addon::encode_classify_event_request(next));
    addon::apply_context_update(state, result.context_updates.front());
    ok &= check(state.provider_session_id == next.ledger.current_transition->provider_session_id &&
            state.used_fraction == 0.25 && !state.compaction_count && state.compaction_event_ids.empty(),
        "a new exact provider session resets prior compaction knowledge and replay identities");
    int exit_code = -1;
    ok &= check(host.shutdown(3000, &exit_code) && exit_code == 0, "context package shuts down cleanly");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "provider-event" ||
            std::string_view(argv[1]) == "request-title-instruction"))
    {
        return helper_transport_fixture(argc, argv);
    }
    if (argc != 2) return 2;
    const auto root = std::filesystem::temp_directory_path() / ("qwen-attention-" + addon::random_nonce_hex());
    int result = 1;
    try {
        std::filesystem::create_directories(root);
        const auto package = addon::path_from_utf8(argv[1]);
        result = launcher_approval_contract() && packaged_settings_and_approval(package, root) &&
                settings_contract() && lifecycle_contract() &&
                declared_profile_admits_every_shape(package) &&
                attention_contract(package, root) && context_contract(package, root)
            ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (!check(!error, "notification fixture workspace removed")) result = 1;
    return result;
}
