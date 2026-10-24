#include "qwen_conversation.h"

#include "qwen_profile.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qwen {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Json = addon::Json_value;
using Block = addon::Conversation_block;
using Kind = addon::Conversation_block_kind;
using Phase = addon::Conversation_message_phase;

bool fail(addon::Contract_failure* failure, std::string_view reason, std::string_view detail)
{
    *failure = {std::string(reason), std::string(detail)};
    return false;
}

bool flag(const Json& value, std::string_view key)
{
    const auto& member = json::member(value, key);
    return member.is_boolean() && member.as_boolean();
}

bool conversation_record(const Json& row)
{
    if (flag(row, "isSidechain")) {
        return false;
    }
    if (json::text(row, "type") != "system") {
        return true;
    }
    const auto subtype = json::text(row, "subtype");
    return subtype != "session_artifact_event" && subtype != "session_artifact_snapshot" &&
        subtype != "session_sources_snapshot" && subtype != "managed_session_header_v1" &&
        subtype != "managed_session_event_v1" && subtype != "managed_session_commit_v1";
}

std::optional<fs::path> source_file(const fs::path& home, std::string_view id,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    std::optional<fs::path> source;
    const auto projects = home / "projects";
    if (fs::is_directory(projects)) {
        const auto name = std::string(id) + ".jsonl";
        for (const auto& project : fs::directory_iterator(projects)) {
            if (cancelled && cancelled()) {
                fail(failure, "cancelled", "Conversation extraction was cancelled.");
                return {};
            }
            const auto chats = project.path() / "chats";
            if (!fs::is_directory(chats)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(chats)) {
                if (addon::path_to_utf8(entry.path().filename()) != name || !entry.is_regular_file()) {
                    continue;
                }
                if (source) {
                    fail(failure, "conversation_source_ambiguous", "Several native files name this Qwen conversation.");
                    return {};
                }
                source = entry.path();
            }
        }
    }
    if (!source) {
        fail(failure, "conversation_source_unavailable", "The native Qwen conversation file is unavailable.");
    }
    return source;
}

bool append_part(const Json& part, std::vector<Block>& blocks, addon::Contract_failure* failure)
{
    if (const auto& text = json::member(part, "text"); text.is_string()) {
        blocks.push_back({Kind::TEXT, text.as_string(), flag(part, "thought") ? "Thinking" : "",
            !flag(part, "thought")});
    }
    else
    if (const auto& call = json::member(part, "functionCall"); call.is_object()) {
        blocks.push_back({Kind::TOOL_CALL, addon::serialize_json(json::member(call, "args")),
            json::text(call, "name") + " " + json::text(call, "id"), false});
    }
    else
    if (const auto& response = json::member(part, "functionResponse"); response.is_object()) {
        const auto& content = json::member(response, "response");
        const auto& output = json::member(content, "output");
        const auto text = output.is_string() && content.as_object().size() == 1 ? output.as_string()
            : content.is_string() ? content.as_string() : addon::serialize_json(content);
        blocks.push_back({Kind::TOOL_RESULT,
            text,
            json::text(response, "name") + " " + json::text(response, "id"), false});
    }
    else
    if (const auto& data = json::member(part, "inlineData"); data.is_object()) {
        blocks.push_back({Kind::ATTACHMENT, "Embedded " + json::text(data, "mimeType") + " attachment", {}});
    }
    else
    if (const auto& data = json::member(part, "fileData"); data.is_object()) {
        const auto uri = json::text(data, "fileUri");
        if (uri.empty()) {
            return fail(failure, "conversation_content_invalid", "A Qwen attachment has no file reference.");
        }
        blocks.push_back({Kind::ATTACHMENT, uri, json::text(data, "mimeType")});
    }
    else
    if (const auto& code = json::member(part, "executableCode"); code.is_object()) {
        blocks.push_back({Kind::TOOL_CALL, json::text(code, "code"), json::text(code, "language"), false});
    }
    else
    if (const auto& result = json::member(part, "codeExecutionResult"); result.is_object()) {
        blocks.push_back({Kind::TOOL_RESULT, json::text(result, "output"), json::text(result, "outcome"), false});
    }
    else
    if (!json::member(part, "thoughtSignature").is_string()) {
        return fail(failure, "conversation_content_unavailable", "A Qwen message part has an unsupported format.");
    }
    return true;
}

bool append_references(const Json& payload, std::vector<Block>& blocks, addon::Contract_failure* failure)
{
    const auto& attachments = json::member(payload, "attachmentReferences");
    if (attachments.is_array()) {
        for (const auto& attachment : attachments.as_array()) {
            const auto id = json::text(attachment, "attachmentId");
            if (id.empty()) {
                return fail(failure, "conversation_content_invalid", "A Qwen attachment has no identity.");
            }
            blocks.push_back({Kind::ATTACHMENT, "attachment: " + id, json::text(attachment, "mimeType")});
        }
    }
    const auto& resources = json::member(payload, "resourceLinks");
    if (resources.is_array()) {
        for (const auto& resource : resources.as_array()) {
            blocks.push_back({Kind::ATTACHMENT, json::text(resource, "uri"), json::text(resource, "name")});
        }
    }
    return true;
}

std::string_view trimmed_native_text(std::string_view text)
{
    // Match the native JavaScript trim() predicate, including its Unicode
    // whitespace and BOM, without treating other control bytes as whitespace.
    static constexpr std::string_view whitespace[] = {
        "\t", "\n", "\v", "\f", "\r", " ", "\u00a0", "\u1680",
        "\u2000", "\u2001", "\u2002", "\u2003", "\u2004", "\u2005",
        "\u2006", "\u2007", "\u2008", "\u2009", "\u200a", "\u2028",
        "\u2029", "\u202f", "\u205f", "\u3000", "\ufeff"
    };
    for (const bool leading : {true, false}) {
        bool removed = true;
        while (removed) {
            removed = false;
            for (const auto space : whitespace) {
                if (leading ? text.starts_with(space) : text.ends_with(space)) {
                    if (leading) {
                        text.remove_prefix(space.size());
                    }
                    else {
                        text.remove_suffix(space.size());
                    }
                    removed = true;
                    break;
                }
            }
        }
    }
    return text;
}

bool complete_hook_context_part(const Json& part)
{
    const auto& value = json::member(part, "text");
    if (!value.is_string()) {
        return false;
    }
    const auto text = trimmed_native_text(value.as_string());
    constexpr std::string_view opening = "<qwen:user-prompt-submit-context>\n";
    constexpr std::string_view closing = "\n</qwen:user-prompt-submit-context>";
    if (!text.starts_with(opening) || !text.ends_with(closing)) {
        return false;
    }
    const auto body_size = text.size() > opening.size() + closing.size()
        ? text.size() - opening.size() - closing.size() : 0;
    const auto body = text.substr(opening.size(), body_size);
    return body.find("<qwen:user-prompt-submit-context>") == std::string_view::npos &&
        body.find("</qwen:user-prompt-submit-context>") == std::string_view::npos;
}

bool compact_user_record(const Json& row)
{
    const auto provenance = json::text(row, "provenance");
    return provenance != "system"           && provenance != "external_agent"  &&
           provenance != "goal_runtime"     && provenance != "goal_control"    &&
           provenance != "execution_output" && provenance != "tool_result";
}

std::map<std::string, Phase> checkpoint_phases(
    const std::vector<Json>& records,
    const std::map<std::string, std::vector<std::size_t>>& fragments,
    const std::vector<std::string>& chain)
{
    std::map<std::string, std::size_t> positions, references;
    std::set<std::size_t> checkpoints;
    for (std::size_t i = 0; i < chain.size(); ++i) {
        positions.emplace(chain[i], i);
        for (const auto index : fragments.at(chain[i])) {
            const auto& row = records[index];
            if (json::text(row, "type") == "system" && json::text(row, "subtype") == "branch_checkpoint") {
                checkpoints.insert(i);
                ++references[json::text(json::member(row, "systemPayload"), "assistantRecordUuid")];
            }
        }
    }
    std::map<std::string, Phase> phases;
    std::size_t boundary = 0;
    for (std::size_t end = 0; end < chain.size(); ++end) {
        const auto& group = fragments.at(chain[end]);
        const auto& row   = records[group.front()];
        if (!checkpoints.contains(end)) {
            continue;
        }
        const auto floor = boundary;
        boundary = end + 1;
        const auto& payload = json::member(row, "systemPayload");
        const auto& version = json::member(payload, "v");
        const auto* start   = payload.find("startExclusiveRecordUuid");
        const auto final_id = json::text(payload, "assistantRecordUuid");
        const auto final    = positions.find(final_id);
        if (group.size() != 1 || end == 0 || json::text(row, "parentUuid") != chain[end - 1] ||
            !version.is_integer() || version.as_integer() != 1 || !start ||
            (!start->is_null() && !start->is_string()) || final == positions.end() ||
            references[final_id] != 1)
        {
            continue;
        }
        std::size_t begin = 0;
        if (start->is_string()) {
            const auto found = positions.find(start->as_string());
            if (start->as_string().empty() || found == positions.end()) {
                continue;
            }
            begin = found->second + 1;
        }
        if (begin < floor || begin >= end || final->second < begin || final->second >= end) {
            continue;
        }
        // Restrict proof to one completed interval with explicit tool IDs.
        // Ambiguous checkpoints and name-only tool matches leave text visible.
        std::set<std::string> issued, pending;
        bool valid = true, saw_user = false, saw_assistant = false;
        std::size_t visible_count = 0, answer = end;
        for (std::size_t i = begin; valid && i < end; ++i) {
            const auto& message_group = fragments.at(chain[i]);
            const auto& first = records[message_group.front()];
            const auto type   = json::text(first, "type");
            bool visible = false, call_present = false, result_present = false;
            if (type == "user" && compact_user_record(first)) {
                valid = !saw_user && !saw_assistant;
                saw_user = true;
            }
            if (type == "assistant") {
                saw_assistant = true;
            }
            for (const auto index : message_group) {
                const auto& fragment = records[index];
                valid = valid && json::member(fragment, "provenance") == json::member(first, "provenance") &&
                    json::member(fragment, "subtype") == json::member(first, "subtype");
                if (type == "system" ||
                    (type == "tool_result" && json::text(fragment, "subtype") == "code_mode_tool_result"))
                {
                    continue;
                }
                const auto& parts = json::member(json::member(fragment, "message"), "parts");
                if (!parts.is_array()) {
                    valid = false;
                    break;
                }
                if (type == "tool_result") {
                    visible_count = 0;
                    result_present = true;
                }
                for (const auto& part : parts.as_array()) {
                    if (const auto* call = part.find("functionCall")) {
                        const auto id = json::text(*call, "id");
                        valid = valid && !id.empty() && issued.insert(id).second;
                        pending.insert(id);
                        call_present = true;
                    }
                    if (const auto* response = part.find("functionResponse")) {
                        const auto id = json::text(*response, "id");
                        valid = valid && !id.empty() && pending.erase(id) == 1;
                        visible_count = 0;
                        result_present = true;
                    }
                    const auto& text = json::member(part, "text");
                    visible = visible || (text.is_string() && !flag(part, "thought") &&
                        !trimmed_native_text(text.as_string()).empty());
                }
            }
            if (type == "assistant" && visible && !call_present && !result_present) {
                ++visible_count;
                answer = i;
            }
        }
        if (!valid || !pending.empty() || visible_count != 1 || answer != final->second) {
            continue;
        }
        for (std::size_t i = begin; i < end; ++i) {
            if (json::text(records[fragments.at(chain[i]).front()], "type") == "assistant") {
                phases[chain[i]] = i == answer ? Phase::FINAL : Phase::COMMENTARY;
            }
        }
    }
    return phases;
}

bool append_record(const Json& row, addon::Conversation_message& message, addon::Contract_failure* failure)
{
    const auto type = json::text(row, "type");
    if (type == "system") {
        if (json::text(row, "subtype") == "agent_bootstrap") {
            return fail(failure, "conversation_format_unavailable",
                "This Qwen conversation retains inherited history in an unsupported agent bootstrap snapshot.");
        }
        return true;
    }
    if (type != "user" && type != "assistant" && type != "tool_result") {
        return fail(failure, "conversation_content_unavailable", "A Qwen transcript record has an unsupported format.");
    }
    message.role = type == "tool_result" ? "tool" : type;
    const auto content_begin = message.content.size();
    const auto& parts = json::member(json::member(row, "message"), "parts");
    if (!parts.is_array()) {
        return fail(failure, "conversation_content_invalid", "A Qwen message has no content parts.");
    }
    const auto& payload = json::member(row, "systemPayload");
    const auto& display = json::member(payload, "displayText");
    const bool final_hook_context = parts.as_array().size() > 1 &&
        complete_hook_context_part(parts.as_array().back());
    const bool display_prompt = type == "user" && display.is_string() &&
        (json::member(payload, "hookContext").is_string() || final_hook_context);
    if (display_prompt) {
        message.content.push_back({Kind::TEXT, display.as_string(), {}});
    }
    for (const auto& part : parts.as_array()) {
        if (type == "assistant" && json::member(part, "functionCall").is_object()) {
            message.phase = Phase::COMMENTARY;
        }
        if (display_prompt && json::member(part, "text").is_string()) {
            continue;
        }
        if (!append_part(part, message.content, failure)) {
            return false;
        }
    }
    if (!append_references(payload, message.content, failure)) {
        return false;
    }
    if (type == "tool_result" || (type == "user" && !compact_user_record(row))) {
        for (auto i = content_begin; i < message.content.size(); ++i) {
            message.content[i].compact_visible = false;
        }
    }
    return true;
}

} // namespace

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    addon::Session_launch_context context;
    context.launch_environment = request.launch_environment;
    context.account_context = request.account_context;
    context.working_directory = request.working_directory;
    const auto source = source_file(profile_for_context(context).home, request.provider_session_id, cancelled, failure);
    if (!source) {
        return {};
    }
    std::vector<Json> records;
    std::map<std::string, std::vector<std::size_t>> fragments;
    std::string leaf;
    const auto snapshot = addon::read_conversation_jsonl(*source,
        [&](const Json& row, addon::Contract_failure* error) {
            if (json::text(row, "sessionId") != request.provider_session_id) {
                return fail(error, "conversation_identity_mismatch", "The Qwen file contains another conversation.");
            }
            if (!conversation_record(row)) {
                return true;
            }
            const auto id = json::text(row, "uuid");
            const auto& parent = json::member(row, "parentUuid");
            if (id.empty() || (!parent.is_string() && !parent.is_null())) {
                return fail(error, "conversation_content_invalid",
                    "A Qwen transcript entry has invalid identity fields.");
            }
            auto& group = fragments[id];
            if (!group.empty()) {
                const auto& first = records[group.front()];
                if (json::text(first, "parentUuid") != json::text(row, "parentUuid") ||
                    json::text(first, "type") != json::text(row, "type"))
                {
                    return fail(error, "conversation_branch_incomplete",
                        "Qwen message fragments disagree on their parent.");
                }
            }
            group.push_back(records.size());
            records.push_back(row);
            leaf = id;
            return true;
        }, cancelled, failure);
    if (!snapshot) {
        return {};
    }
    std::vector<std::string> chain;
    std::set<std::string> visited;
    while (!leaf.empty()) {
        const auto found = fragments.find(leaf);
        if (found == fragments.end() || !visited.insert(leaf).second) {
            fail(failure, "conversation_branch_incomplete",
                "The retained Qwen conversation has a broken parent chain.");
            return {};
        }
        chain.push_back(leaf);
        leaf = json::text(records[found->second.front()], "parentUuid");
    }
    std::reverse(chain.begin(), chain.end());
    const auto phases = checkpoint_phases(records, fragments, chain);
    addon::Conversation_writer writer(request, cancelled);
    if (!writer.open(failure)) {
        return {};
    }
    bool has_messages = false;
    for (const auto& id : chain) {
        addon::Conversation_message message;
        if (const auto found = phases.find(id); found != phases.end()) {
            message.phase = found->second;
        }
        for (const auto index : fragments.at(id)) {
            if (!append_record(records[index], message, failure)) {
                return {};
            }
        }
        if (message.phase == Phase::COMMENTARY) {
            for (auto& block : message.content) {
                block.compact_visible = false;
            }
        }
        if (!message.content.empty()) {
            has_messages = true;
            if (!writer.append(message, failure)) {
                return {};
            }
        }
    }
    if (!has_messages) {
        fail(failure, "conversation_content_unavailable", "No retained Qwen conversation messages are available.");
        return {};
    }
    return writer.finish(snapshot->incomplete_tail_omitted, failure);
}

} // namespace qwen
