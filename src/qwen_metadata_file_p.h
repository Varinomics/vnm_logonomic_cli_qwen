#pragma once

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace qwen::detail {

struct Metadata_file_close
{
    void operator()(std::FILE* input) const noexcept { std::fclose(input); }
};

using Metadata_file = std::unique_ptr<std::FILE, Metadata_file_close>;

inline std::optional<std::string> read_metadata_file(
    const std::filesystem::path& io_path, std::size_t maximum_bytes)
{
    std::string bytes(maximum_bytes + 1, '\0');
    std::FILE* input = nullptr;
#ifdef _WIN32
    // Permit delete sharing, but Windows rename-over can still fail while
    // this handle is open. Retain only the bytes after the bounded read.
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
    const Metadata_file file(input);
    if (!file) {
        return std::nullopt;
    }
    bytes.resize(std::fread(bytes.data(), 1, bytes.size(), file.get()));
    if (std::ferror(file.get()) || bytes.size() > maximum_bytes) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace qwen::detail
