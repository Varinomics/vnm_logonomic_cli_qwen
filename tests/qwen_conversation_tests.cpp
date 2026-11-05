#include "qwen_conversation.h"
#include "qwen_profile.h"
#include "windows_filesystem_test_support.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/package_source.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
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

bool source_lookup(const fs::path& root, addon::Conversation_export_request request)
{
    const auto profile = root / "lookup-profile";
    const auto project = profile / "projects/project-a" / "chats";
    const auto other   = profile / "projects/project-b" / "chats";
    const auto source  = project / "session-a.jsonl";
    request.launch_environment.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)};
    request.output_directory = addon::path_to_utf8(root / "lookup-output");
    const auto write_session = [](
            const fs::path& path,
            std::string_view id)
        {
            write(path, R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":)" +
                addon::serialize_json(addon::Json_value(std::string(id))) + "," +
                R"("message":{"role":"user","parts":[{"text":"Lookup content"}]}})" "\n");
        };

    addon::Contract_failure failure;
    const auto extract = [
            &
        ](
            const std::function<bool()>& cancelled = {})
        {
            const auto output = addon::path_from_utf8(request.output_directory);
            fs::remove_all(output);
            fs::create_directories(output);
            return qwen::export_conversation(request, cancelled, &failure);
        };
    bool ok = check(!extract() &&
        failure.reason_code == "conversation_source_unavailable", "missing history is unavailable");
    write_session(source, "session-a");
    fs::create_directories(other);
    ok &= check(extract().has_value(),
        "a known transcript is found among projects without matching files");

    write_session(project.parent_path() / "session-a.jsonl", "../session-a");
    write_session(project / "nested/session-a.jsonl", "nested/session-a");
    for (const auto* invalid : {"../session-a", "nested/session-a", "nested\\session-a",
             "C:session-a", "session-a:stream"})
    {
        request.provider_session_id = invalid;
        ok &= check(!extract() &&
            failure.reason_code == "conversation_source_unavailable",
            "a session identity cannot select a path, drive or stream");
    }
    const auto absolute = addon::path_to_utf8(root / "absolute-session");
    write_session(addon::path_from_utf8(absolute + ".jsonl"), absolute);
    request.provider_session_id = absolute;
    ok &= check(!extract() &&
        failure.reason_code == "conversation_source_unavailable", "absolute session paths are refused");

    request.provider_session_id = "session-a";
    unsigned cancellation_checks = 0;
    ok &= check(!extract([&] { return ++cancellation_checks == 2; }) &&
        failure.reason_code == "cancelled" && cancellation_checks == 2,
        "cancellation remains observable between project candidates");
    write_session(other / "session-a.jsonl", "session-a");
    ok &= check(!extract() &&
        failure.reason_code == "conversation_source_ambiguous", "matching projects remain ambiguous");
    fs::remove(other / "session-a.jsonl");

    fs::remove(source);
    fs::create_directory(source);
    ok &= check(!extract() &&
        failure.reason_code == "conversation_source_unavailable", "a directory is not a transcript");
    fs::remove(source);

    const auto target = root / "linked-transcript.jsonl";
    write_session(target, "session-a");
    std::error_code link_error;
    fs::create_symlink(target, source, link_error);
    if (link_error == std::errc::permission_denied ||
        link_error == std::errc::operation_not_supported
#ifdef _WIN32
        || link_error.value() == ERROR_PRIVILEGE_NOT_HELD
#endif
    ) {
        std::cout << "SKIP: transcript symlinks unavailable: " << link_error.message() << '\n';
    }
    else
    if (check(!link_error, "create the transcript symlink")) {
        ok &= check(extract().has_value(),
            "a matching regular-file symlink remains readable");
        fs::remove(target);
        ok &= check(!extract() &&
            failure.reason_code == "conversation_source_unavailable", "a dangling link is a nonmatch");
        fs::remove(source);
        fs::create_symlink(source, source);
        bool status_failed = false;
        try {
            extract();
        }
        catch (const fs::filesystem_error&) {
            status_failed = true;
        }
        ok &= check(status_failed, "a candidate status error is not hidden as missing history");
        fs::remove(source);
    }
    else {
        ok = false;
    }

    // Native filename lookup decides aliases; transcript identity remains exact.
    const auto uppercase = project / "SESSION-A.jsonl";
    write_session(uppercase, "session-a");
    const bool case_alias = fs::is_regular_file(source);
    const auto aliased = extract();
    ok &= check(aliased.has_value() == case_alias &&
        (case_alias || failure.reason_code == "conversation_source_unavailable"),
        "filename aliases follow this filesystem's case policy");
    request.provider_session_id = "SESSION-A";
    ok &= check(!extract() &&
        failure.reason_code == "conversation_identity_mismatch", "session content is never case folded");
    write_session(uppercase, "SESSION-A");
    ok &= check(extract().has_value(),
        "native uppercase identity and filename spelling are preserved");
    write_session(other / "session-a.jsonl", "session-a");
    const auto other_alias = extract();
    ok &= check(other_alias.has_value() != case_alias &&
        (!case_alias || failure.reason_code == "conversation_source_ambiguous"),
        "case aliases in another project participate in ambiguity detection");
    return ok;
}

bool export_equivalence(const fs::path& root)
{
    const auto profile = root / addon::path_from_utf8("equivalence-profile-Ω");
    const auto source  = profile / "projects/project-a/chats/session-a.jsonl";
    write(source,
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":"session-a","message":{"role":"user","parts":[{"text":"Question Ω\nline"}]}})" "\n"
        R"({"type":"assistant","uuid":"reply","parentUuid":"prompt","sessionId":"session-a","message":{"role":"model","parts":[{"text":"Answer 中文"}]}})" "\n");
    addon::Conversation_export_request request;
    request.provider_session_id = "session-a";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)};
    bool ok = true;
    for (const auto format : {addon::Conversation_export_format::MARKDOWN,
             addon::Conversation_export_format::TEXT, addon::Conversation_export_format::CONVERSATION})
    {
        const std::string expected = format == addon::Conversation_export_format::MARKDOWN
            ? "## User\n\nQuestion Ω\nline\n\n## Assistant\n\nAnswer 中文\n\n"
            : format == addon::Conversation_export_format::TEXT
                ? "User\n\nQuestion Ω\nline\n\nAssistant\n\nAnswer 中文\n\n"
                : "#!vnm-chat 1\n\n::: user\nQuestion Ω\nline\n:::\n\n::: assistant\nAnswer 中文\n:::\n\n";
        for (const bool compact : {false, true}) {
            request.format = format;
            request.compact = compact;
            const auto output = root / "exports" / (compact ? "compact" : "full") /
                addon::conversation_export_format_name(format);
            fs::create_directories(output);
            request.output_directory = addon::path_to_utf8(output);
            addon::Contract_failure failure;
            const auto result = qwen::export_conversation(request, {}, &failure);
            ok &= check(result.has_value(), failure.reason_code + " " + failure.detail);
            if (result) {
                const auto final = output / result->file_name;
                ok &= check(read(final) == expected && result->bytes == expected.size() &&
                    result->message_count == 2 && result->attachment_count == 0 &&
                    !result->incomplete_tail_omitted && result->compact == compact,
                    "complete exports match exact UTF-8 bytes and message counts");
                ok &= check(!fs::exists(output / (result->file_name + ".partial")),
                    "successful close and final rename leave only the completed output");
                std::cout << "EXPORT " << addon::conversation_export_format_name(format)
                    << " compact=" << compact << " messages=" << result->message_count
                    << " bytes=" << result->bytes << '\n';
            }
        }
    }
    return ok;
}

bool native_filesystem(const fs::path& root)
{
    const auto profile = root / addon::path_from_utf8("native-profile-用户-Ω");
    const auto chats   = profile / "projects/project-a/chats";
    const auto output  = root / "native-output";
    addon::Conversation_export_request request;
    request.provider_session_id = "会话-Ω-🙂";
    request.launch_environment.platform_id = addon::host_platform_id();
    request.launch_environment.bootstrap = addon::Environment_bootstrap_evidence{addon::path_to_utf8(profile)};
    request.output_directory = addon::path_to_utf8(output);
    request.format = addon::Conversation_export_format::TEXT;
    const auto source = chats / addon::path_from_utf8(request.provider_session_id + ".jsonl");
    const auto body = R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":)" +
        addon::serialize_json(addon::Json_value(request.provider_session_id)) + "," +
        R"("message":{"role":"user","parts":[{"text":"Native Unicode Ω 中文 🙂"}]}})" "\n";
    write(source, body);
    addon::Contract_failure failure;
    const auto extract = [
            &
        ](
            const std::function<bool()>& cancelled = {})
        {
            fs::remove_all(output);
            fs::create_directories(output);
            return qwen::export_conversation(request, cancelled, &failure);
        };
    bool ok = check(extract().has_value(), "Unicode native filename and profile resolve without byte loss");
    request.account_context = addon::Account_context{};
    request.account_context->adapter_account_key = qwen::account_key(profile);
    request.account_context->profile_path = addon::path_to_utf8(profile);
    ok &= check(extract().has_value(), "matching original profile and selected account export");
    request.account_context->adapter_account_key = qwen::account_key(root / "another-profile");
    bool mismatched = false;
    try {
        extract();
    }
    catch (const std::runtime_error& error) {
        mismatched = std::string_view(error.what()) == "account_profile_mismatch";
    }
    ok &= check(mismatched && fs::is_empty(output), "another account cannot select the transcript");
    request.account_context.reset();

    const std::string uppercase_id = "9F1A87B2-3C4D-4E5F-8A9B-0123456789AB";
    const auto uppercase_source = chats / (uppercase_id + ".jsonl");
    write(uppercase_source,
        R"({"type":"user","uuid":"prompt","parentUuid":null,"sessionId":)" +
        addon::serialize_json(addon::Json_value(uppercase_id)) + "," +
        R"("message":{"role":"user","parts":[{"text":"Uppercase native UUID"}]}})" "\n");
    request.provider_session_id = uppercase_id;
    ok &= check(extract().has_value(), "an uppercase native Qwen UUID remains exact");
    request.provider_session_id = "会话-Ω-🙂";

#ifdef _WIN32
    {
        filesystem_test::Access_denial denial(chats, FILE_LIST_DIRECTORY);
        std::error_code error;
        const fs::directory_iterator denied(chats, error);
        ok &= check(error == std::errc::permission_denied, "the native ACL actually denies directory enumeration");
        ok &= check(read(source) == body && extract().has_value(),
            "a known readable transcript exports without listing its chats directory");
    }
    {
        filesystem_test::Access_denial denial(source, FILE_READ_DATA);
        std::ifstream denied(source, std::ios::binary);
        ok &= check(!denied, "the native ACL actually denies the known file read");
        ok &= check(!extract() && failure.reason_code == "conversation_unavailable" && fs::is_empty(output),
            "a real snapshot read denial is reported without a completed output");
    }
    ok &= check(!filesystem_test::restoration_failed, "temporary fixture ACLs are restored");
    const auto stream = chats / "stream-base:transcript.jsonl";
    write(chats / "stream-base", "base file");
    write(stream, body);
    ok &= check(read(stream) == body, "the native alternate stream fixture is present");
    request.provider_session_id = "stream-base:transcript";
    ok &= check(!extract() && failure.reason_code == "conversation_source_unavailable",
        "a real alternate stream cannot become a transcript candidate");
    request.provider_session_id = "会话-Ω-🙂";
#endif

    bool final_blocked = false;
    const auto final   = output / addon::conversation_export_file_name(request.format);
    const auto partial = output / (std::string(addon::conversation_export_file_name(request.format)) + ".partial");
    const auto failed_rename = extract([&] {
        if (!final_blocked && fs::exists(partial)) {
            fs::create_directory(final);
            final_blocked = true;
        }
        return false;
    });
    ok &= check(final_blocked && !failed_rename && failure.reason_code == "conversation_export_failed" &&
        fs::is_directory(final) && !fs::exists(partial),
        "a genuine final rename failure reports failure and removes partial output");
    bool writing_started = false;
    const auto cancelled = extract([&] {
        writing_started = writing_started || fs::exists(partial);
        return writing_started;
    });
    ok &= check(writing_started && !cancelled && failure.reason_code == "cancelled" && fs::is_empty(output),
        "cancellation during output writing removes the partial export");
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--export-equivalence") {
        const auto destination = addon::path_from_utf8(argv[2]);
        fs::create_directories(destination);
        return export_equivalence(fs::canonical(destination)) ? 0 : 1;
    }
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
    ok &= source_lookup(root, request);
    ok &= export_equivalence(root);
    ok &= native_filesystem(root);
    fs::remove_all(root);
    return ok ? 0 : 1;
}
