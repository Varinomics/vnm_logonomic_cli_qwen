#include "qwen_launcher.h"
#include "qwen_json.h"
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <regex>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace qwen {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
std::string trim(std::string_view value)
{
    std::size_t first = value.size(), last = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto begin = offset;
        const auto lead = static_cast<unsigned char>(value[offset++]);
        std::uint32_t scalar = lead;
        const auto continuation = lead >= 0xF0 ? 3u : lead >= 0xE0 ? 2u : lead >= 0xC0 ? 1u : 0u;
        if (continuation != 0 && offset + continuation <= value.size()) {
            scalar = lead & ((1u << (6u - continuation)) - 1u);
            for (unsigned index = 0; index < continuation; ++index) {
                scalar = (scalar << 6) | (static_cast<unsigned char>(value[offset++]) & 0x3F);
            }
        }
        const bool whitespace = scalar == 0x20 || (scalar >= 9 && scalar <= 13) || scalar == 0x85 ||
            scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) ||
            scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
        if (!whitespace) { first = std::min(first, begin); last = offset; }
    }
    return last == 0 ? std::string{} : std::string(value.substr(first, last - first));
}

std::string lower(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return value;
}

std::string unquote(std::string_view value)
{
    std::string result = trim(value);
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') ||
         (result.front() == '"'  && result.back() == '"')))
    {
        return result.substr(1, result.size() - 2);
    }
    return result;
}

std::string base_name(std::string_view value)
{
    std::string name = lower(unquote(value));
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name.erase(0, separator + 1);
    for (const std::string_view suffix : {".exe", ".cmd", ".ps1", ".bat"}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

std::string path_string(const fs::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

std::string environment_value(const Environment& environment, std::string_view name)
{
    for (const auto& [key, value] : environment) if (equal_name(key, name)) return value;
    return {};
}

void set_environment(Environment& environment, std::string name, std::string value)
{
    std::erase_if(environment, [&](const auto& entry) { return equal_name(entry.first, name); });
    environment.emplace_back(std::move(name), std::move(value));
}

std::optional<fs::path> regular_file(const fs::path& path)
{
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    return !error && fs::is_regular_file(canonical, error) && !error
        ? std::optional(canonical) : std::nullopt;
}

std::optional<fs::path> resolve_program(
    std::string_view program, const Environment& environment, std::string_view working_directory)
{
    const auto path = addon::path_from_utf8(program);
    const bool has_working_directory = addon::path_from_utf8(working_directory).is_absolute();
    const auto usable_program = [](const fs::path& candidate) -> std::optional<fs::path> {
        const auto file = regular_file(candidate);
#ifndef _WIN32
        if (file && access(file->c_str(), X_OK) != 0) return std::nullopt;
#endif
        return file;
    };
    if (path.is_absolute()) return usable_program(path);
    if (path.has_parent_path()) {
        return has_working_directory ? usable_program(addon::path_from_utf8(working_directory) / path) : std::nullopt;
    }
    std::vector<fs::path> candidates;
    {
        const auto search_path = environment_value(environment, "PATH");
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::istringstream parts(search_path);
        for (std::string part; std::getline(parts, part, separator);) {
            if (part.empty()) continue;
            auto directory = addon::path_from_utf8(unquote(part));
            if (!directory.is_absolute()) {
                if (!has_working_directory) continue;
                directory = addon::path_from_utf8(working_directory) / directory;
            }
            candidates.push_back(directory / path);
        }
    }
    for (const auto& candidate : candidates) {
#ifdef _WIN32
        if (!candidate.has_extension()) {
            auto extensions = environment_value(environment, "PATHEXT");
            if (std::none_of(environment.begin(), environment.end(), [](const auto& entry) {
                    return equal_name(entry.first, "PATHEXT");
                })) extensions = ".COM;.EXE;.BAT;.CMD";
            std::istringstream parts(extensions);
            for (std::string extension; std::getline(parts, extension, ';');) {
                extension = trim(extension);
                if (extension.empty()) continue;
                if (!extension.starts_with('.')) extension.insert(extension.begin(), '.');
                if (const auto file = usable_program(addon::path_from_utf8(path_string(candidate) + extension))) return file;
            }
            continue;
        }
#endif
        if (const auto file = usable_program(candidate)) return file;
    }
    return std::nullopt;
}

bool native_image(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 4> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
#ifdef _WIN32
    return file.gcount() >= 2 && header[0] == 'M' && header[1] == 'Z';
#else
    if (file.gcount() != 4) return false;
    const auto magic = (static_cast<std::uint32_t>(header[0]) << 24) |
        (static_cast<std::uint32_t>(header[1]) << 16) |
        (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
    return magic == 0x7f454c46 || magic == 0xfeedface || magic == 0xfeedfacf ||
        magic == 0xcefaedfe || magic == 0xcffaedfe || magic == 0xcafebabe || magic == 0xbebafeca;
#endif
}

#ifdef _WIN32
std::wstring wide(std::string_view text)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size == 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif

std::optional<Probe_output> probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, addon::Invocation_context& context)
{
    if (context.cancel_requested()) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    Probe_output result;
    bool valid = true;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output_read, output_write, errors_read, errors_write;
    if (!CreatePipe(&output_read.value, &output_write.value, &security, 0) ||
        !CreatePipe(&errors_read.value, &errors_write.value, &security, 0)) return std::nullopt;
    SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errors_read.value, HANDLE_FLAG_INHERIT, 0);
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) return std::nullopt;
    std::wstring command;
    for (const auto& argument : argv) {
        if (!command.empty()) command += L' ';
        command += quote_argument(wide(argument));
    }
    std::vector<std::wstring> variables;
    for (const auto& [name, value] : environment) variables.push_back(wide(name + "=" + value));
    std::sort(variables.begin(), variables.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::wstring environment_block;
    for (const auto& variable : variables) { environment_block += variable; environment_block += L'\0'; }
    environment_block += L'\0';
    if (variables.empty()) environment_block += L'\0';
    // Restrict handle inheritance: concurrent adapter invokes must not retain
    // each other's pipe handles and keep a completed probe alive.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) return std::nullopt;
    HANDLE handles[]{input.value, output_write.value, errors_write.value};
    const bool updated = UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = input.value;
    startup.StartupInfo.hStdOutput = output_write.value;
    startup.StartupInfo.hStdError  = errors_write.value;
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION information{};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const auto directory = wide(working_directory);
    const bool created = updated && job.value &&
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
        CreateProcessW(wide(argv.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment_block.data(), directory.empty() ? nullptr : directory.c_str(),
            &startup.StartupInfo, &information);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created) return std::nullopt;
    Handle process{information.hProcess}, thread{information.hThread};
    CloseHandle(output_write.value); output_write.value = nullptr;
    CloseHandle(errors_write.value); errors_write.value = nullptr;
    if (!AssignProcessToJobObject(job.value, process.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return std::nullopt;
    }
    const auto drain = [&](HANDLE pipe, std::string& output) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 4096> buffer{};
            DWORD count = 0;
            if (!ReadFile(pipe, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) break;
            if (output.size() + count > 65536) { valid = false; break; }
            output.append(buffer.data(), count);
        }
    };
    for (;;) {
        drain(output_read.value, result.output);
        drain(errors_read.value, result.errors);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        context.wait_for_cancel(10);
    }
    TerminateJobObject(job.value, 1);
    drain(output_read.value, result.output);
    drain(errors_read.value, result.errors);
    DWORD exit_code = 1;
    valid = GetExitCodeProcess(process.value, &exit_code) && exit_code == 0 && valid;
#else
    int output_pipe[2], errors_pipe[2];
    if (pipe2(output_pipe, O_CLOEXEC) != 0) return std::nullopt;
    if (pipe2(errors_pipe, O_CLOEXEC) != 0) {
        close(output_pipe[0]); close(output_pipe[1]); return std::nullopt;
    }
    const int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        for (int descriptor : {output_pipe[0], output_pipe[1], errors_pipe[0], errors_pipe[1]}) close(descriptor);
        return std::nullopt;
    }
    std::vector<std::string> environment_strings;
    for (const auto& [name, value] : environment) environment_strings.push_back(name + "=" + value);
    std::vector<char*> arguments, variables;
    for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
    for (auto& variable : environment_strings) variables.push_back(variable.data());
    arguments.push_back(nullptr);
    variables.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors_pipe[1], STDERR_FILENO);
    const std::string directory(working_directory);
    if (!directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
    pid_t process = 0;
    const int spawned = posix_spawn(&process, argv.front().c_str(), &actions, &attributes,
        arguments.data(), variables.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input); close(output_pipe[1]); close(errors_pipe[1]);
    if (spawned != 0) { close(output_pipe[0]); close(errors_pipe[0]); return std::nullopt; }
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errors_pipe[0], F_SETFL, O_NONBLOCK);
    const auto drain = [&](int descriptor, std::string& output) {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = read(descriptor, buffer.data(), buffer.size());
            if (count <= 0) break;
            if (output.size() + static_cast<std::size_t>(count) > 65536) { valid = false; break; }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    };
    int status = 0;
    bool reaped = false;
    bool child_owned = true;
    for (;;) {
        drain(output_pipe[0], result.output);
        drain(errors_pipe[0], result.errors);
        const auto waited = waitpid(process, &status, WNOHANG);
        if (waited == process) { reaped = true; child_owned = false; break; }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD) child_owned = false;
            valid = false;
            break;
        }
        if (!valid || context.cancel_requested() || std::chrono::steady_clock::now() >= deadline) {
            valid = false;
            break;
        }
        pollfd descriptors[]{{output_pipe[0], POLLIN, 0}, {errors_pipe[0], POLLIN, 0}};
        poll(descriptors, 2, 10);
    }
    if (child_owned && !reaped) {
        // The unreaped leader still reserves its PID. Never signal after a
        // successful reap or ECHILD, when that number can identify a new task.
        // Descendants retain the custodian's group; creating a private group
        // here would violate the SDK's no-detach contract.
        kill(process, SIGKILL);
        pid_t waited;
        do { waited = waitpid(process, &status, 0); } while (waited < 0 && errno == EINTR);
        reaped = waited == process;
    }
    drain(output_pipe[0], result.output);
    drain(errors_pipe[0], result.errors);
    close(output_pipe[0]); close(errors_pipe[0]);
    valid = valid && reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    return valid && !context.cancel_requested() ? std::optional(std::move(result)) : std::nullopt;
}

Environment environment_pairs(const Json& value)
{
    Environment result;
    for (const auto& entry : value.as_object()) result.emplace_back(entry.name, entry.value.as_string());
    return result;
}

std::vector<std::string> literal_argv(std::string_view script)
{
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    int quotes = 0;
    for (const char character : script) {
        if (character == '"') {
            if (++quotes == 3) { current += '"'; quotes = 0; }
            continue;
        }
        if (quotes == 1) quoted = !quoted;
        quotes = 0;
        if (!quoted && std::isspace(static_cast<unsigned char>(character))) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
        }
        else current += character;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

bool is_qwen_invocation(const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        return false;
    }
    if (base_name(argv.front()) == "qwen") {
        return true;
    }
    const auto shell = base_name(argv.front());
    for (std::size_t index = 1; index + 1 < argv.size(); ++index) {
        const auto flag = lower(argv[index]);
        const bool command = ((shell == "powershell" || shell == "pwsh") && flag == "-command") ||
            (shell == "cmd" && flag == "/c") || ((shell == "bash" || shell == "sh") && flag == "-c");
        if (command && index + 2 == argv.size()) {
            return recognize_launcher(argv.back());
        }
    }
    return false;
}

bool recognize_launcher(std::string_view script)
{
    if (script.find_first_of(";&|<>$`%\r\n") != std::string_view::npos) {
        return false;
    }
    const auto argv = literal_argv(script);
    return !argv.empty() && base_name(argv.front()) == "qwen";
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    const auto environment = environment_pairs(request.ambient_environment);
    const auto program = resolve_program("qwen", environment, request.default_home_fallback);
    if (!program) {
        return std::nullopt;
    }
    addon::Launcher_candidate result;
    result.candidate_id = "qwen";
    result.display_name = "Qwen Code";
    // npm installs a qwen symlink to a JavaScript entry point. Retain the
    // command identity; later preparation resolves the same frozen PATH.
    result.script_body = "qwen";
    result.runner_key = "direct";
    result.is_default = true;
    return result;
}

std::optional<std::vector<std::string>> platform_command(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory)
{
    if (argv.empty()) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto extension = lower(addon::path_from_utf8(argv.front()).extension().string());
    if (extension == ".cmd" || extension == ".bat" || extension == ".ps1") {
        const auto shell = resolve_program("powershell.exe", environment, working_directory);
        if (!shell || !native_image(*shell)) {
            return std::nullopt;
        }
        std::string script = "&";
        for (const auto& argument : argv) {
            // cmd wrappers perform a second expansion pass. Do not feed that
            // pass shell syntax through an otherwise literal process contract.
            if (extension != ".ps1" && argument.find_first_of("\"%&|<>^!()\r\n") != std::string::npos) {
                return std::nullopt;
            }
            script += " '";
            for (const auto character : argument) {
                script += character;
                if (character == '\'') {
                    script += '\'';
                }
            }
            script += '\'';
        }
        script += "; exit $LASTEXITCODE";
        return std::vector<std::string>{path_string(*shell), "-NoLogo", "-NoProfile",
            "-NonInteractive", "-Command", std::move(script)};
    }
    if (!native_image(addon::path_from_utf8(argv.front()))) {
        return std::nullopt;
    }
#else
    (void)environment;
    (void)working_directory;
#endif
    return argv;
}

std::optional<std::vector<std::string>> selected_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory)
{
    if (launcher.runner_key != "direct" || !recognize_launcher(launcher.script_body) || argv.empty())
    {
        return std::nullopt;
    }
    const auto literal = literal_argv(launcher.script_body);
    if (literal.size() != argv.size() ||
        !std::equal(literal.begin() + 1, literal.end(), argv.begin() + 1))
    {
        return std::nullopt;
    }
    const auto selected = resolve_program(argv.front(), environment, working_directory);
    const auto authored = resolve_program(literal.front(), environment, working_directory);
    if (!selected || !authored || !equal_name(path_string(*selected), path_string(*authored))) {
        return std::nullopt;
    }
    auto command = argv;
    command.front() = path_string(*selected);
    return command;
}

std::optional<std::vector<std::string>> verified_command(
    const addon::Launcher_request& launcher, const std::vector<std::string>& argv,
    const Environment& environment, std::string_view working_directory,
    std::string_view required_option, addon::Invocation_context& context)
{
    const auto selected_argv = selected_command(launcher, argv, environment, working_directory);
    if (!selected_argv) {
        return std::nullopt;
    }
    const auto& command = *selected_argv;
    const auto selected = addon::path_from_utf8(command.front());
    const auto probe_command = platform_command({command.front(), "--help"}, environment, working_directory);
    if (!probe_command) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = fs::file_size(selected, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = fs::last_write_time(selected, error);
    if (error) {
        return std::nullopt;
    }
    const auto output = probe(*probe_command, environment, working_directory, 10000, context);
    if (!output || fs::file_size(selected, error) != size || error ||
        fs::last_write_time(selected, error) != modified || error)
    {
        return std::nullopt;
    }
    // Capability evidence comes from the selected executable, never a ceiling
    // on a moving vendor release number.
    const std::string help = output->output + '\n' + output->errors;
    const auto option = help.find(required_option);
    if (option == std::string::npos ||
        (option > 0 && help[option - 1] != ' ' && help[option - 1] != '\t') ||
        (option + required_option.size() < help.size() &&
            std::isalnum(static_cast<unsigned char>(help[option + required_option.size()]))))
    {
        return std::nullopt;
    }
    return command;
}

} // namespace qwen
