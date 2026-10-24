#include "qwen_lifecycle.h"
#include "qwen_json.h"

#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

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
    const auto session = json_util::text(body, "session_id");
    const auto agent = json_util::text(body, "agent_id");
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
    observation.provider_event_id = json_util::text(body, "event_id");
    if (!identity(observation.provider_event_id)) {
        result.lifecycle_degradation = addon::Lifecycle_degradation{1, session};
        return result;
    }
    const auto turn = json_util::text(body, "prompt_id");
    if (!turn.empty() && identity(turn)) {
        observation.provider_turn_id = turn;
    }
    if (name == "SessionStart") {
        const auto source = json_util::text(body, "source");
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
        observation.resume_record = json_util::object({{"provider_session_id", Json(session)}});
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
            observation.session_end_reason = json_util::text(body, "reason");
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
        if (name == "Notification" && (json_util::text(body, "notification_type") == "permission_prompt" ||
            json_util::text(body, "notification_type") == "elicitation_dialog"))
        {
            observation.attention_transition = addon::Attention_transition::REQUESTED;
            observation.attention_evidence = addon::Evidence::INFERRED;
        }
        else {
            return {};
        }
    }
    addon::Contract_failure failure;
    if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation),
            &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS))
    {
        result = {};
        result.lifecycle_degradation = addon::Lifecycle_degradation{1, session};
        return result;
    }
    result.observations.push_back(std::move(observation));
    return result;
}

addon::Attention_classification_result classify_attention(const addon::Attention_classification_request& request)
{
    addon::Attention_classification_result result{classify_event(request.base), std::nullopt};
    if (request.base.event.name != "Notification" || result.base.observations.size() != 1 ||
        result.base.observations.front().attention_transition != addon::Attention_transition::REQUESTED)
    {
        return result;
    }
    const auto notification = json_util::text(request.base.event.body, "notification_type");
    if (notification == "permission_prompt" || notification == "elicitation_dialog") {
        // Native notifications carry no paired question identity or answer.
        // Label the existing inferred observation without upgrading its evidence.
        result.attention_kind = addon::Attention_kind{0,
            notification == "permission_prompt" ? "permission" : "interactive_question"};
    }
    return result;
}

} // namespace qwen
