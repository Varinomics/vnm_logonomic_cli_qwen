#pragma once

#include <logonomic_cli_addon/json.h>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace qwen {

using Json = logonomic::cli_addon::Json_value;

inline const Json& member(const Json& value, std::string_view key)
{
    static const Json absent;
    const auto* found = value.find(key);
    return found ? *found : absent;
}

inline std::string text(const Json& value, std::string_view key)
{
    const auto& found = member(value, key);
    return found.is_string() ? found.as_string() : std::string{};
}

inline Json object(std::initializer_list<logonomic::cli_addon::Json_member> members)
{
    return Json(Json::Object(members));
}

inline bool only_keys(const Json& value, std::initializer_list<std::string_view> keys)
{
    return value.is_object() && std::all_of(value.as_object().begin(), value.as_object().end(),
        [&](const auto& entry) { return std::find(keys.begin(), keys.end(), entry.name) != keys.end(); });
}

inline bool exact_keys(const Json& value, std::initializer_list<std::string_view> keys)
{
    return only_keys(value, keys) && value.as_object().size() == keys.size();
}

inline bool bounded_string(const Json& value, std::size_t maximum, bool empty = false)
{
    return value.is_string() && (empty || !value.as_string().empty()) &&
        value.as_string().size() <= maximum && value.as_string().find('\0') == std::string::npos;
}

inline std::optional<Json> parse(std::string_view bytes)
{
    return logonomic::cli_addon::parse_json(bytes, {}, nullptr);
}

} // namespace qwen
