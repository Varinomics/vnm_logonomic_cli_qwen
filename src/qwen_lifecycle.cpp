#include "qwen_lifecycle.h"
#include "qwen_json.h"

#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;

bool identity(std::string_view value)
{
    return !value.empty() && value.size() <= addon::limits::k_max_lifecycle_identity_bytes &&
        value.find('\0') == std::string_view::npos && addon::is_valid_utf8(value);
}

} // namespace

addon::Classify_event_result classify_event(const addon::Classify_event_request& request)
{
    addon::Classify_event_result result;
    const auto& body = request.event.body;
    const auto session = text(body, "session_id");
    const auto agent = text(body, "agent_id");
    const auto& name = request.event.name;
    if (!identity(session)) {
        result.lifecycle_degradation = addon::Lifecycle_degradation{};
        return result;
    }
    if (!agent.empty()) {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = session;
    observation.provider_event_id = text(body, "event_id");
    if (!identity(observation.provider_event_id)) {
        result.lifecycle_degradation = addon::Lifecycle_degradation{1, session};
        return result;
    }
    const auto turn = text(body, "prompt_id");
    if (!turn.empty() && identity(turn)) {
        observation.provider_turn_id = turn;
    }
    if (name == "SessionStart") {
        const auto source = text(body, "source");
        if (source != "startup" && source != "resume" && source != "branch" && source != "clear") {
            // Compaction preserves the Config session identity. Clear and
            // branch rotate it before the caller initializes the next chat.
            return result;
        }
        if (source == "resume" && request.ledger.current_transition &&
            request.ledger.current_transition->provider_session_id == session)
        {
            return result;
        }
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source = source;
        observation.resume_record = object({{"provider_session_id", Json(session)}});
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "resume"
            ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
    }
    else {
        if (!request.ledger.current_transition ||
            request.ledger.current_transition->provider_session_id != session)
        {
            return result;
        }
        result.classification = addon::Classification::EVENT;
        if (name == "SessionEnd") {
            observation.session_transition = addon::Session_transition::ENDED;
            observation.session_end_reason = text(body, "reason");
        }
        else
        if (name == "UserPromptSubmit") {
            observation.phase_transition = addon::Phase_transition::WORKING;
            observation.phase_evidence = addon::Evidence::INFERRED;
        }
        else
        if (name == "Stop" || name == "StopFailure") {
            // Other native hooks can block Stop and continue the same turn.
            // Receipt is phase evidence, not proof of an accepted outcome.
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::INFERRED;
            if (name == "StopFailure" && !observation.provider_turn_id.empty()) {
                // Unlike Stop, this native notification ignores hook output
                // after an API error or loop detector ended the turn.
                observation.outcome_transition = addon::Outcome_transition::FAILED;
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
        else
        if (name == "Notification" && (text(body, "notification_type") == "permission_prompt" ||
            text(body, "notification_type") == "elicitation_dialog"))
        {
            observation.attention_transition = addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
        }
        else {
            return {};
        }
    }
    addon::Contract_failure failure;
    if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation), &failure)) {
        result = {};
        result.lifecycle_degradation = addon::Lifecycle_degradation{1, session};
        return result;
    }
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace qwen
