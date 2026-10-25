#include "qwen_config.h"
#include "qwen_launcher.h"
#include "qwen_launcher_approval.h"
#include "qwen_lifecycle.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sidecar.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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

bool launcher_approval_contract()
{
    const auto& rules = qwen::launcher_approval_rules();
    const auto yolo = addon::Launcher_approval_action::YOLOFY;
    const auto undo = addon::Launcher_approval_action::UNYOLOFY;
    const auto changed = addon::Launcher_approval_disposition::CHANGED;
    const auto windows = addon::transform_launcher_approval({
        {"qwen", "direct", ""}, "windows-x86_64", yolo}, rules);
    bool ok = check(windows.disposition == changed && windows.script_body == "qwen --yolo",
        "discovered Windows Qwen launcher supports Yolofy");
    for (const auto* policy : {"-y", "--approval-mode=yolo", "--approval-mode auto-edit"}) {
        const auto result = addon::transform_launcher_approval({
            {std::string("qwen ") + policy + " --model custom", "direct", ""}, "linux-x86_64", yolo}, rules);
        ok &= check(result.disposition == changed && result.script_body == "qwen --yolo --model custom",
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
        ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "Yolofy cannot turn a Qwen positional prompt into a boolean operand");
    }
    for (const auto action : {yolo, undo}) {
        const auto result = addon::transform_launcher_approval({
            {"qwen --sandbox --approval-mode yolo false", "direct", ""}, "linux-x86_64", action}, rules);
        ok &= check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "approval editing cannot expose a Qwen prompt to another boolean flag");
    }
    const auto delimited = addon::transform_launcher_approval({
        {"qwen -- false", "direct", ""}, "linux-x86_64", yolo}, rules);
    ok &= check(delimited.disposition == changed && delimited.script_body == "qwen --yolo -- false",
        "an explicit delimiter preserves the Qwen false prompt while enabling bypass");
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

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const auto root = std::filesystem::temp_directory_path() / ("qwen-attention-" + addon::random_nonce_hex());
    int result = 1;
    try {
        std::filesystem::create_directories(root);
        const auto package = addon::path_from_utf8(argv[1]);
        result = launcher_approval_contract() && settings_contract() && lifecycle_contract() &&
                declared_profile_admits_every_shape(package) &&
                attention_contract(package, root)
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
