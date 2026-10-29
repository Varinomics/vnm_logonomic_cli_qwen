#include "qwen_conversation.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

bool check(bool condition, std::string_view detail)
{
    if (!condition) {
        std::cerr << "FAIL: " << detail << '\n';
    }
    return condition;
}

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t occurrences(std::string_view haystack, std::string_view needle)
{
    std::size_t count = 0;
    for (auto offset = haystack.find(needle); offset != std::string_view::npos;
         offset = haystack.find(needle, offset + needle.size()))
    {
        ++count;
    }
    return count;
}

bool compact_projection(const fs::path& root, const fs::path& source,
    addon::Conversation_export_request request)
{
    const std::string body =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","parts":[{"text":"Visible prompt"},{"fileData":{"fileUri":"file:///tmp/retained.png","mimeType":"image/png"}}]}})" "\n"
        R"({"type":"assistant","uuid":"progress","parentUuid":"prompt","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Intermediate progress"}]}})" "\n"
        R"({"type":"assistant","uuid":"call","parentUuid":"progress","sessionId":"session-a","message":{"role":"model","parts":[{"functionCall":{"id":"read-a","name":"read_file","args":{"path":"note.txt"}}}]}})" "\n"
        R"({"type":"tool_result","uuid":"result","parentUuid":"call","sessionId":"session-a","message":{"role":"user","parts":[{"functionResponse":{"id":"read-a","name":"read_file","response":{"output":"Tool result text"}}}]}})" "\n"
        R"({"type":"user","uuid":"internal","parentUuid":"result","sessionId":"session-a","provenance":"system","message":{"role":"user","parts":[{"text":"Injected system text"}]}})" "\n"
        R"({"type":"assistant","uuid":"final","parentUuid":"internal","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Reasoning detail","thought":true},{"text":"Visible final reply"}]}})" "\n"
        R"({"type":"system","subtype":"branch_checkpoint","uuid":"checkpoint","parentUuid":"final","sessionId":"session-a","systemPayload":{"v":1,"startExclusiveRecordUuid":null,"assistantRecordUuid":"final"}})" "\n";
    write(source, body);
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            request.output_directory = addon::path_to_utf8(root / "projection" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure failure;
            const auto result = qwen::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
            if (!result) {
                continue;
            }
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->compact == compact && result->message_count == (compact ? 2 : 6) &&
                result->attachment_count == 1, "compact retains the user and completed final reply with its attachment");
            for (const auto* retained : {"Visible prompt", "Visible final reply", "file:///tmp/retained.png"}) {
                ok &= check(occurrences(text, retained) == 1, std::string(retained) + " retained once");
            }
            for (const auto* hidden : {"Intermediate progress", "Tool result text", "Injected system text", "Reasoning detail"}) {
                ok &= check((text.find(hidden) == std::string::npos) == compact,
                    "the completed turn excludes tool activity, injected messages, and reasoning only in compact");
            }
            if (format == addon::Conversation_export_format::CONVERSATION) {
                ok &= check(text.find("::: assistant final\nVisible final reply") != std::string::npos,
                    "a native branch checkpoint proves the exported final phase");
            }
        }
    }
    return ok;
}

} // namespace

int main()
{
    // Oracle: Qwen ChatRecord and prepareTranscriptRecords: UUID fragments
    // accumulate parts, parentUuid selects the latest branch, forkedFrom is
    // audit metadata. Primary schema links: docs/conversation_export.md.
    const auto scratch = fs::temp_directory_path() / ("qwen-conversation-" + addon::random_nonce_hex());
    fs::create_directories(scratch);
    const auto root = fs::canonical(scratch);
    const auto profile = root / "profile";
    const auto source = profile / "projects/project-a/chats/session-a.jsonl";
    const std::string body =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","forkedFrom":{"sessionId":"parent","messageUuid":"prompt"},"message":{"role":"user","parts":[{"text":"expanded model prompt"},{"text":"hook context"},{"fileData":{"fileUri":"file:///tmp/image.png","mimeType":"image/png"}}]},"systemPayload":{"displayText":"Original user prompt Ω","hookContext":"hook context"}})" "\n"
        R"({"type":"assistant","uuid":"reply","parentUuid":"prompt","sessionId":"session-a","message":{"role":"model","parts":[{"text":"First fragment"}]}})" "\n"
        R"({"type":"assistant","uuid":"reply","parentUuid":"prompt","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Second fragment"}]}})" "\n"
        R"({"type":"assistant","uuid":"call","parentUuid":"reply","sessionId":"session-a","message":{"role":"model","parts":[{"functionCall":{"id":"read-a","name":"read_file","args":{"path":"note.txt"}}}]}})" "\n"
        R"({"type":"tool_result","uuid":"result","parentUuid":"call","sessionId":"session-a","message":{"role":"user","parts":[{"functionResponse":{"id":"read-a","name":"read_file","response":{"output":"Complete tool output\nwith newline"}}}]}})" "\n"
        R"({"type":"user","uuid":"old-user","parentUuid":"result","sessionId":"session-a","message":{"role":"user","parts":[{"text":"Discarded prompt"}]}})" "\n"
        R"({"type":"assistant","uuid":"old-reply","parentUuid":"old-user","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Discarded reply"}]}})" "\n"
        R"({"type":"system","subtype":"rewind","uuid":"rewind","parentUuid":"result","sessionId":"session-a","systemPayload":{}})" "\n"
        R"({"type":"user","uuid":"current-user","parentUuid":"rewind","sessionId":"session-a","message":{"role":"user","parts":[{"text":"Current prompt"}]}})" "\n"
        R"({"type":"assistant","uuid":"current-reply","parentUuid":"current-user","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Current reply"}]}})" "\n"
        R"({"type":"system","subtype":"ui_telemetry","uuid":"usage","parentUuid":"current-reply","sessionId":"session-a","systemPayload":{"uiEvent":{"event.name":"qwen-code.api_response"}}})" "\n";
    write(source, body + "{\"uuid\":");
    addon::Conversation_export_request request;
    request.provider_session_id = "session-a";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)};
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::TEXT,
             addon::Conversation_export_format::CONVERSATION})
    {
        request.format = format;
        request.output_directory = addon::path_to_utf8(root / addon::conversation_export_format_name(format));
        fs::create_directories(addon::path_from_utf8(request.output_directory));
        addon::Contract_failure failure;
        const auto result = qwen::export_conversation(request, {}, &failure);
        ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
        if (result) {
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            ok &= check(result->message_count == 6 && result->attachment_count == 1, "fragments form one native message");
            ok &= check(result->incomplete_tail_omitted, "in-progress partial tail reported");
            for (const auto* expected : {"Original user prompt Ω", "First fragment", "Second fragment", "Complete tool output"}) {
                ok &= check(occurrences(text, expected) == 1, std::string(expected) + " retained once");
            }
            ok &= check(text.find("expanded model prompt") == std::string::npos &&
                text.find("hook context") == std::string::npos, "native display prompt replaces injected model context");
            ok &= check(text.find("Discarded") == std::string::npos, "rewind selects current native branch");
            ok &= check(text.find("First fragment") < text.find("Second fragment") &&
                text.find("Second fragment") < text.find("Complete tool output"), "part and tool order retained");
        }
    }
    // Native projectUserTranscriptForDisplay accepts released displayText-only
    // metadata when its final part is a complete, non-nested hook wrapper.
    const std::string released_display =
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","parts":[{"text":"Expanded released model prompt"},{"fileData":{"fileUri":"file:///tmp/released-image.png","mimeType":"image/png"}},{"text":" \t<qwen:user-prompt-submit-context>\nInjected released hook text\n</qwen:user-prompt-submit-context>\u00a0\ufeff"}]},"systemPayload":{"displayText":"Original released prompt Ω"}})" "\n"
        R"({"type":"assistant","uuid":"answer","parentUuid":"prompt","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Released answer"}]}})" "\n";
    const std::string unpaired_display =
        R"({"type":"user","uuid":"incomplete","parentUuid":null,"sessionId":"session-a","message":{"role":"user","parts":[{"text":"Preserve incomplete prompt"},{"text":"<qwen:user-prompt-submit-context>\nIncomplete hook"}]},"systemPayload":{"displayText":"Unproved display A"}})" "\n"
        R"({"type":"user","uuid":"nonfinal","parentUuid":"incomplete","sessionId":"session-a","message":{"role":"user","parts":[{"text":"<qwen:user-prompt-submit-context>\nNonfinal hook\n</qwen:user-prompt-submit-context>"},{"text":"Preserve trailing prompt"}]},"systemPayload":{"displayText":"Unproved display B"}})" "\n"
        R"({"type":"user","uuid":"nested","parentUuid":"nonfinal","sessionId":"session-a","message":{"role":"user","parts":[{"text":"Preserve nested prompt"},{"text":"<qwen:user-prompt-submit-context>\n<qwen:user-prompt-submit-context>Nested hook\n</qwen:user-prompt-submit-context>"}]},"systemPayload":{"displayText":"Unproved display C"}})" "\n"
        R"({"type":"user","uuid":"single","parentUuid":"nested","sessionId":"session-a","message":{"role":"user","parts":[{"text":"<qwen:user-prompt-submit-context>\nPreserve single part\n</qwen:user-prompt-submit-context>"}]},"systemPayload":{"displayText":"Unproved display D"}})" "\n";
    for (const bool paired : {true, false}) {
        write(source, paired ? released_display : unpaired_display);
        for (const auto format : {addon::Conversation_export_format::MARKDOWN,
                 addon::Conversation_export_format::TEXT})
        {
            request.format = format;
            request.output_directory = addon::path_to_utf8(root /
                (paired ? "released-display" : "unpaired-display") / addon::conversation_export_format_name(format));
            fs::create_directories(addon::path_from_utf8(request.output_directory));
            addon::Contract_failure error;
            const auto result = qwen::export_conversation(request, {}, &error);
            ok &= check(result.has_value(), error.reason_code + " " + error.detail);
            if (!result) {
                continue;
            }
            const auto text = read(addon::path_from_utf8(request.output_directory) / result->file_name);
            if (paired) {
                ok &= check(result->message_count == 2 && result->attachment_count == 1 &&
                    occurrences(text, "Original released prompt Ω") == 1 &&
                    occurrences(text, "file:///tmp/released-image.png") == 1,
                    "released display metadata preserves the original prompt and non-text attachment");
                ok &= check(text.find("Expanded released model prompt") == std::string::npos &&
                    text.find("Injected released hook text") == std::string::npos,
                    "complete final hook part proves the released display projection");
            }
            else {
                ok &= check(result->message_count == 4 && text.find("Unproved display") == std::string::npos,
                    "unpaired display metadata never replaces native text");
                for (const auto* expected : {"Preserve incomplete prompt", "Preserve trailing prompt",
                         "Preserve nested prompt", "Preserve single part"})
                {
                    ok &= check(occurrences(text, expected) == 1,
                        std::string(expected) + " remains without native pairing evidence");
                }
            }
        }
    }
    ok &= compact_projection(root, source, request);
    request.output_directory = addon::path_to_utf8(root / "failures");
    fs::create_directories(addon::path_from_utf8(request.output_directory));
    addon::Contract_failure failure;
    write(source, R"({"type":"user","uuid":"a","parentUuid":"missing","sessionId":"session-a","message":{"role":"user","parts":[{"text":"orphan"}]}})" "\n");
    ok &= check(!qwen::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_branch_incomplete", "missing history never silently truncates");
    write(source, body +
        R"({"type":"assistant","uuid":"reply","parentUuid":"another","sessionId":"session-a","message":{"role":"model","parts":[{"text":"conflict"}]}})" "\n");
    ok &= check(!qwen::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_branch_incomplete", "conflicting UUID fragments fail");
    write(source, R"({"type":"user","uuid":"a","parentUuid":null,"sessionId":"another","message":{"role":"user","parts":[{"text":"wrong"}]}})" "\n");
    ok &= check(!qwen::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_identity_mismatch", "exact source identity enforced");
    write(source,
        R"({"type":"system","subtype":"agent_bootstrap","uuid":"bootstrap","parentUuid":null,"sessionId":"session-a","systemPayload":{"kind":"fork","history":[{"role":"user","parts":[{"text":"Inherited task"}]}]}})" "\n"
        R"({"type":"assistant","uuid":"answer","parentUuid":"bootstrap","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Child answer"}]}})" "\n");
    ok &= check(!qwen::export_conversation(request, {}, &failure) &&
        failure.reason_code == "conversation_format_unavailable", "unsupported bootstrap history is not silently omitted");
    write(source, body + "malformed\n");
    ok &= check(!qwen::export_conversation(request, {}, &failure), "malformed completed JSONL does not become success");
    write(source, body);
    ok &= check(!qwen::export_conversation(request, [] { return true; }, &failure) &&
        failure.reason_code == "cancelled", "cancelled scan emits no export");
    fs::remove_all(root);
    return ok ? 0 : 1;
}
