#include "qwen_lifecycle.h"
#include "qwen_json.h"
#include "qwen_resume.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/context_state.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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

std::optional<std::uint64_t> context_tokens(const Json& body, std::string_view key, double minimum)
{
    const auto* value = body.find(key);
    if (!value || !value->is_number()) {
        return std::nullopt;
    }
    const double tokens = value->as_number();
    if (!std::isfinite(tokens) || tokens < minimum || tokens > 9007199254740991.0 ||
        std::floor(tokens) != tokens)
    {
        return std::nullopt;
    }
    return (std::uint64_t)tokens;
}

void classify_start_directory(
    const addon::Classify_event_request& request, addon::Classify_event_result& result,
    const std::string& session)
{
    if (request.execution_context_version != addon::k_execution_context_version || request.execution_failure) {
        return;
    }
    addon::Execution_context_update update;
    update.provider_session_id = session;
    update.transition_nonce    = request.transition_nonce;
    update.source              = "qwen.SessionStart";
    const auto* cwd = request.event.body.find("cwd");
    if (cwd && cwd->is_string() && addon::is_execution_local_path(cwd->as_string())) {
        update.status      = addon::Execution_context_status::CONFIRMED;
        update.environment = "local";
        update.cwd         = cwd->as_string();
    }
    // This native root snapshot establishes the default directory only for
    // this activation. Launch inputs and transcript paths cannot fill a gap.
    result.execution_updates.push_back(std::move(update));
}

} // namespace

addon::Classify_event_result classify_event(const addon::Classify_event_request& request)
{
    addon::Classify_event_result result;
    result.execution_failure = request.execution_failure;
    const auto& body = request.event.body;
    const auto session = json_util::text(body, "session_id");
    const auto agent = json_util::text(body, "agent_id");
    const auto& name = request.event.name;
    if (request.context_failure) {
        result.context_failure = addon::Context_failure{{}, *request.context_failure};
        if (agent.empty() && identity(session) &&
            std::none_of(session.begin(), session.end(), [](unsigned char byte) { return byte < 0x20; }))
        {
            result.context_failure->provider_session_id = session;
        }
    }
    if (!identity(session)) {
        result.lifecycle_degradation = addon::Lifecycle_degradation{};
        return result;
    }
    if (!agent.empty()) {
        return result;
    }
    if (name == "SessionEnd" && json_util::text(body, "reason") == "clear") {
        // Qwen fires this reset notification without awaiting it. The next
        // SessionStart owns turnover; a late clear end could otherwise end
        // a new activation of the same native ID after A -> B -> A.
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
    if (request.context_state_version == addon::k_context_state_capability_version &&
        request.ledger.current_transition &&
        request.ledger.current_transition->provider_session_id == session)
    {
        const bool compacted = name == "SessionStart" && json_util::text(body, "source") == "compact";
        if (name == "Stop" || compacted) {
            addon::Context_update update;
            update.provider_session_id = session;
            update.event_id            = observation.provider_event_id;
            update.compacted           = compacted;
            if (name == "Stop") {
                // Native Stop reports the current prompt and its configured
                // capacity, independently of cumulative usage accounting.
                update.used_tokens    = context_tokens(body, "input_tokens", 0);
                update.context_window = context_tokens(body, "context_limit", 1);
                if (!update.used_tokens || !update.context_window) {
                    update.used_tokens.reset();
                    update.context_window.reset();
                    update.usage_unavailable = true;
                }
            }
            result.context_updates.push_back(std::move(update));
        }
    }
    const auto turn = json_util::text(body, "prompt_id");
    // Native clear/resume can retain the departed conversation's prompt_id
    // while emitting SessionStart for the newly selected conversation.
    if (name != "SessionStart" && !turn.empty() && identity(turn)) {
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
        observation.resume_record = make_resume_record(session, body);
        result.classification = addon::Classification::TRANSITION;
        result.transition = observation.session_source == "resume"
            ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        classify_start_directory(request, result, session);
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
            result.classification = addon::Classification::IGNORED;
            return result;
        }
    }
    addon::Contract_failure failure;
    if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation),
            &failure, addon::Lifecycle_observation_policy::HELPER_INGRESS))
    {
        auto context_failure = std::move(result.context_failure);
        result = {};
        result.context_failure = std::move(context_failure);
        result.execution_failure = request.execution_failure;
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
