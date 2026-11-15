#pragma once

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace qwen::detail {

struct Metadata_file_close
{
    void operator()(std::FILE* input) const noexcept { std::fclose(input); }
};

using Metadata_file = std::unique_ptr<std::FILE, Metadata_file_close>;

inline Metadata_file open_metadata_file(const std::filesystem::path& io_path)
{
    std::FILE* input = nullptr;
#ifdef _WIN32
    // Qwen publishes each snapshot by rename-over. The reader must not make
    // that best-effort write fail while it retains the preceding snapshot.
    const auto handle = CreateFileW(io_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (descriptor == -1) {
            CloseHandle(handle);
        }
        else {
            input = _fdopen(descriptor, "rb");
            if (!input) {
                _close(descriptor);
            }
        }
    }
#else
    input = std::fopen(io_path.c_str(), "rb");
#endif
    return Metadata_file(input);
}

} // namespace qwen::detail
