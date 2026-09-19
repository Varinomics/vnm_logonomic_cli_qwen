#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstddef>
#include <string>
#include <vector>

#ifdef _WIN32
namespace {

std::wstring quote_argument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
        }
        else {
            result.append(slashes, L'\\');
        }
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        return 2;
    }
    std::wstring command;
    for (int i = 1; i < argc; ++i) {
        if (!command.empty()) {
            command += L' ';
        }
        command += quote_argument(argv[i]);
    }
    HANDLE inherited[3]{};
    const DWORD identifiers[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(identifiers[i]), GetCurrentProcess(),
                &inherited[i], 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            for (int j = 0; j < i; ++j) {
                CloseHandle(inherited[j]);
            }
            return 3;
        }
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    bool started = false;
    PROCESS_INFORMATION process{};
    if (initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr))
    {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb          = sizeof(startup);
        startup.StartupInfo.dwFlags     = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput   = inherited[0];
        startup.StartupInfo.hStdOutput  = inherited[1];
        startup.StartupInfo.hStdError   = inherited[2];
        startup.lpAttributeList        = attributes;
        started = CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    }
    if (initialized) {
        DeleteProcThreadAttributeList(attributes);
    }
    for (const HANDLE handle : inherited) {
        CloseHandle(handle);
    }
    if (!started) {
        return 3;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 3;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    else {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    ExitProcess(exit_code);
}
#else
int main(int argc, char** argv)
{
    if (argc < 2) {
        return 2;
    }
    // exec preserves the host-owned process group, all three streams and exit semantics.
    execv(argv[1], argv + 1);
    return 3;
}
#endif
