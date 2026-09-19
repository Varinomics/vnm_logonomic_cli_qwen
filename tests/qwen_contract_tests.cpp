#include "qwen_config.h"
#include "qwen_launcher.h"
#include "qwen_lifecycle.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
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
    ok &= check(qwen::text(qwen::member(qwen::member(parsed, "security"), "auth"), "selectedType") == "openai",
        "selected native authentication survives");
    const auto& stop = qwen::member(qwen::member(parsed, "hooks"), "Stop");
    ok &= check(stop.is_array() && stop.as_array().size() == 2,
        "existing Stop command and managed command coexist");
    ok &= check(qwen::updated_config(configured) == configured, "hook installation is idempotent");
    ok &= check(configured.find("/* before */") != std::string::npos &&
        configured.find("/* after */") != std::string::npos, "hook-array comments survive");
    auto modified = parsed;
    auto hooks = qwen::member(modified, "hooks");
    auto modified_stop = qwen::member(hooks, "Stop");
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
    request.event.body = qwen::object({{"session_id", qwen::Json("12345678-1234-4234-8234-123456789abc")},
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

} // namespace

int main()
{
    try {
        return settings_contract() && lifecycle_contract() ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
