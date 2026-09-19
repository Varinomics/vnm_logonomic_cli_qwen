#include "qwen_config.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qwen {
namespace {

constexpr const char* k_events[] = {
    "SessionStart", "SessionEnd", "UserPromptSubmit", "Stop", "StopFailure",
    "Notification",
};

struct value_span_t
{
    std::string name;
    std::size_t begin;
    std::size_t end;
};

class Settings_document
{
public:
    explicit Settings_document(std::string_view source) : m_source(source), m_json(source)
    {
        bool quoted = false;
        for (std::size_t index = 0; index < m_json.size(); ++index) {
            if (quoted) {
                if (m_json[index] == '\\') {
                    ++index;
                }
                else
                if (m_json[index] == '"') {
                    quoted = false;
                }
                continue;
            }
            if (m_json[index] == '"') {
                quoted = true;
            }
            else
            if (m_json[index] == '/' && index + 1 < m_json.size()) {
                if (m_json[index + 1] == '/') {
                    while (index < m_json.size() && m_json[index] != '\n') {
                        m_json[index++] = ' ';
                    }
                    if (index == m_json.size()) {
                        break;
                    }
                }
                else
                if (m_json[index + 1] == '*') {
                    const auto end = m_json.find("*/", index + 2);
                    if (end == std::string::npos) {
                        throw std::runtime_error("invalid_settings_comment");
                    }
                    std::fill(m_json.begin() + index, m_json.begin() + end + 2, ' ');
                    index = end + 1;
                }
            }
        }
        m_without_comments = m_json;
        quoted = false;
        for (std::size_t index = 0; index < m_json.size(); ++index) {
            if (quoted) {
                if (m_json[index] == '\\') {
                    ++index;
                }
                else
                if (m_json[index] == '"') {
                    quoted = false;
                }
            }
            else
            if (m_json[index] == '"') {
                quoted = true;
            }
            else
            if (m_json[index] == ',') {
                const auto next = skip(index + 1);
                if (next < m_json.size() && (m_json[next] == '}' || m_json[next] == ']')) {
                    m_json[index] = ' ';
                }
            }
        }
        const auto parsed = parse(m_json);
        if (!parsed || !parsed->is_object()) {
            throw std::runtime_error("invalid_settings_object");
        }
        m_value = *parsed;
    }

    const Json& value() const { return m_value; }

    std::size_t skip(std::size_t cursor) const
    {
        while (cursor < m_json.size() && std::isspace(static_cast<unsigned char>(m_json[cursor]))) {
            ++cursor;
        }
        return cursor;
    }

    std::size_t end(std::size_t cursor) const
    {
        if (m_json.at(cursor) == '"') {
            for (++cursor; cursor < m_json.size(); ++cursor) {
                if (m_json[cursor] == '\\') {
                    ++cursor;
                }
                else
                if (m_json[cursor] == '"') {
                    return cursor + 1;
                }
            }
        }
        else
        if (m_json.at(cursor) == '{' || m_json.at(cursor) == '[') {
            const char closing = m_json[cursor] == '{' ? '}' : ']';
            cursor = skip(cursor + 1);
            while (m_json.at(cursor) != closing) {
                if (m_json[cursor] == ':' || m_json[cursor] == ',') {
                    cursor = skip(cursor + 1);
                }
                else {
                    cursor = skip(end(cursor));
                }
            }
            return cursor + 1;
        }
        else {
            while (cursor < m_json.size() && m_json[cursor] != ',' && m_json[cursor] != '}' &&
                m_json[cursor] != ']' && !std::isspace(static_cast<unsigned char>(m_json[cursor])))
            {
                ++cursor;
            }
            return cursor;
        }
        throw std::runtime_error("invalid_settings_value");
    }

    std::vector<value_span_t> members(std::size_t opening) const
    {
        std::vector<value_span_t> result;
        auto cursor = skip(opening + 1);
        while (m_json.at(cursor) != '}') {
            const auto key_end = end(cursor);
            const auto key = parse(std::string_view(m_json).substr(cursor, key_end - cursor));
            cursor = skip(skip(key_end) + 1);
            const auto value_end = end(cursor);
            result.push_back({key->as_string(), cursor, value_end});
            cursor = skip(value_end);
            if (m_json.at(cursor) == ',') {
                cursor = skip(cursor + 1);
            }
        }
        return result;
    }

    std::string hooks_array(const value_span_t& span, const Json& array, std::string_view event) const
    {
        if (!array.is_array()) {
            throw std::runtime_error("invalid_hooks_array");
        }
        const auto expected = logonomic::cli_addon::serialize_json_canonical(hook_entry(event));
        bool found = false;
        for (const auto& entry : array.as_array()) {
            const auto& hooks = member(entry, "hooks");
            if (hooks.is_array()) {
                for (const auto& hook : hooks.as_array()) {
                    if (text(hook, "name") == "logonomic:qwen:" + std::string(event)) {
                        // Ownership covers the whole exact definition, including
                        // its matcher and execution policy, never a name alone.
                        if (found || logonomic::cli_addon::serialize_json_canonical(entry) != expected) {
                            throw std::runtime_error("modified_managed_hook");
                        }
                        found = true;
                    }
                }
            }
        }
        if (found) {
            return m_source.substr(span.begin, span.end - span.begin);
        }
        const auto closing = span.end - 1;
        auto previous = closing;
        while (previous > span.begin && std::isspace(static_cast<unsigned char>(m_without_comments[previous - 1]))) {
            --previous;
        }
        const bool comma = previous > span.begin + 1 && m_without_comments[previous - 1] != ',';
        return m_source.substr(span.begin, closing - span.begin) + (comma ? "," : "") + expected + ']';
    }

    std::string insert_members(std::size_t opening, std::string_view additions) const
    {
        const auto closing = end(opening) - 1;
        auto previous = closing;
        while (previous > opening && std::isspace(static_cast<unsigned char>(m_without_comments[previous - 1]))) {
            --previous;
        }
        const bool comma = previous > opening + 1 && m_without_comments[previous - 1] != ',';
        return m_source.substr(0, closing) + (comma ? "," : "") +
            std::string(additions) + m_source.substr(closing);
    }

private:
    std::string m_source;
    std::string m_json;
    std::string m_without_comments;
    Json        m_value;
};

} // namespace

Json hook_entry(std::string_view event)
{
#ifdef _WIN32
    const char* shell = "powershell";
    const char* command = "if ($env:LOGONOMIC_PROVIDER_HELPERS_DIR) { & (Join-Path $env:LOGONOMIC_PROVIDER_HELPERS_DIR 'qwen_helper.exe') }";
#else
    const char* shell = "bash";
    const char* command = "if [ -n \"$LOGONOMIC_PROVIDER_HELPERS_DIR\" ]; then \"$LOGONOMIC_PROVIDER_HELPERS_DIR/qwen_helper\"; fi";
#endif
    return object({{"hooks", Json(Json::Array{object({{"type", Json("command")},
        {"name", Json("logonomic:qwen:" + std::string(event))}, {"command", Json(command)},
        {"shell", Json(shell)}, {"timeout", Json(25.0)}})})}});
}

Json parse_settings(std::string_view settings)
{
    return Settings_document(settings.empty() ? "{}" : settings).value();
}

std::string updated_config(std::string_view settings)
{
    const std::string source = settings.empty() ? "{}" : std::string(settings);
    const Settings_document document(source);
    const auto& root = document.value();
    const auto& disabled = member(root, "disableAllHooks");
    if (disabled.is_boolean() && disabled.as_boolean()) {
        throw std::runtime_error("hooks_disabled_by_user");
    }
    auto members = document.members(document.skip(0));
    const auto found = std::find_if(members.begin(), members.end(), [](const auto& entry) {
        return entry.name == "hooks";
    });
    const auto& hooks = member(root, "hooks");
    if (found != members.end() && !hooks.is_object()) {
        throw std::runtime_error("invalid_hooks_object");
    }
    std::string additions;
    std::string replacement = found == members.end() ? "{}" : source.substr(found->begin, found->end - found->begin);
    const Settings_document hook_document(replacement);
    auto hook_members = hook_document.members(hook_document.skip(0));
    std::vector<std::pair<value_span_t, std::string>> edits;
    for (const auto* event : k_events) {
        const auto item = std::find_if(hook_members.begin(), hook_members.end(), [&](const auto& entry) {
            return entry.name == event;
        });
        if (item != hook_members.end()) {
            edits.emplace_back(*item, hook_document.hooks_array(*item, member(hooks, event), event));
        }
        else {
            if (!additions.empty()) {
                additions += ',';
            }
            additions += '"' + std::string(event) + "\":[" +
                logonomic::cli_addon::serialize_json_canonical(hook_entry(event)) + ']';
        }
    }
    std::sort(edits.begin(), edits.end(), [](const auto& left, const auto& right) {
        return left.first.begin > right.first.begin;
    });
    for (const auto& [span, bytes] : edits) {
        replacement.replace(span.begin, span.end - span.begin, bytes);
    }
    if (!additions.empty()) {
        const Settings_document edited(replacement);
        replacement = edited.insert_members(edited.skip(0), additions);
    }
    if (found == members.end()) {
        return document.insert_members(document.skip(0), "\"hooks\":" + replacement);
    }
    auto result = source;
    result.replace(found->begin, found->end - found->begin, replacement);
    return result;
}

bool has_managed_hooks(std::string_view settings)
{
    try {
        return updated_config(settings) == settings;
    }
    catch (const std::exception&) {
        return false;
    }
}

} // namespace qwen
