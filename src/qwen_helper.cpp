#include "qwen_json.h"
#include "qwen_launcher.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/ids.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace json_util = logonomic::cli_addon::json;

namespace addon = logonomic::cli_addon;

namespace {

bool changed_files_requested(const qwen::Json& input)
{
    auto prompt = json_util::text(input, "submitted_prompt");
    if (prompt.empty()) {
        prompt = json_util::text(input, "prompt");
    }
    prompt = qwen::lower(std::move(prompt));
    bool presentation = false;
    bool changes = false;
    for (const auto* verb : {"show", "open", "view", "display", "bring up", "pull up"}) {
        presentation = presentation || prompt.find(verb) != std::string::npos;
    }
    for (const auto* phrase : {"changed file", "modified file", "edited file", "my changes", "your changes",
        "changes you made", "files you changed", "files you modified", "files you edited"})
    {
        changes = changes || prompt.find(phrase) != std::string::npos;
    }
    return presentation && changes;
}

} // namespace

int main()
{
    const auto environment = addon::read_helper_environment(nullptr);
    if (!environment || environment->package_id != "logonomic.cli.qwen") {
        return 0;
    }
    std::string bytes;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), stdin);
        if (bytes.size() + count > 4 * 1024 * 1024) {
            return 1;
        }
        bytes.append(buffer.data(), count);
        if (count != buffer.size()) {
            if (std::ferror(stdin)) {
                return 1;
            }
            break;
        }
    }
    addon::Json_parse_limits limits;
    limits.max_string_bytes = 4 * 1024 * 1024;
    const auto input = addon::parse_json(bytes, limits, nullptr);
    if (!input || !input->is_object()) {
        return 1;
    }
    const auto event = json_util::text(*input, "hook_event_name");
    if (event != "SessionStart" && event != "SessionEnd" && event != "UserPromptSubmit" &&
        event != "Stop" && event != "StopFailure" && event != "Notification")
    {
        return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(21);
    addon::Helper_client client(*environment, [&] { return std::chrono::steady_clock::now() >= deadline; });
    auto body = qwen::Json::object();
    for (const auto* key : {"session_id", "prompt_id", "agent_id", "source", "reason", "notification_type"}) {
        const auto value = json_util::text(*input, key);
        if (!value.empty()) {
            body.set(key, qwen::Json(value));
        }
    }
    if (event == "Stop") {
        for (const auto* key : {"input_tokens", "context_limit"}) {
            if (const auto* value = input->find(key); value && value->is_number()) {
                body.set(key, *value);
            }
        }
    }
    const auto delivery = addon::random_nonce_hex();
    body.set("event_id", qwen::Json(delivery));
    const auto nonce = addon::random_nonce_hex();
    auto result = client.deliver_provider_event(event, body, delivery, nonce);
    if (!result.committed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        result = client.deliver_provider_event(event, body, delivery, nonce);
    }
    if (result.committed) {
        client.acknowledge_provider_event(delivery);
    }
    if ((event == "SessionStart" || event == "UserPromptSubmit") && json_util::text(*input, "agent_id").empty()) {
        const bool changed_files = event == "UserPromptSubmit" && changed_files_requested(*input);
        auto instruction = client.request_title_instruction("qwen", event, changed_files, addon::Model_context_delivery{});
        if (!instruction.completed && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            instruction = client.request_title_instruction("qwen", event, changed_files, addon::Model_context_delivery{});
        }
        if (instruction.completed && instruction.accepted && !instruction.instruction.empty()) {
            const auto output = json_util::object({{"hookSpecificOutput", json_util::object({
                {"hookEventName", qwen::Json(event)}, {"additionalContext", qwen::Json(instruction.instruction)}})}});
            const auto serialized = addon::serialize_json_canonical(output);
            std::fwrite(serialized.data(), 1, serialized.size(), stdout);
            std::fputc('\n', stdout);
        }
    }
    return 0;
}
