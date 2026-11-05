#pragma once

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace filesystem_test {

inline bool restoration_failed = false;

// The deny ACE applies only to the isolated fixture itself, so denying a
// directory listing does not remove the known transcript's read permission.
class Access_denial
{
public:
    Access_denial(const std::filesystem::path& path, DWORD rights)
    :
        m_path(path.native())
    {
        const auto queried = GetNamedSecurityInfoW(m_path.c_str(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &m_original_acl, nullptr, &m_descriptor);
        if (queried != ERROR_SUCCESS) {
            throw std::system_error((int)queried, std::system_category(), "read fixture ACL");
        }
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(m_descriptor, &control, &revision)) {
            const auto error = GetLastError();
            LocalFree(m_descriptor);
            m_descriptor = nullptr;
            throw std::system_error((int)error, std::system_category(), "read fixture ACL control");
        }
        m_protected = (control & SE_DACL_PROTECTED) != 0;
        BYTE world[SECURITY_MAX_SID_SIZE];
        DWORD bytes = sizeof(world);
        if (!CreateWellKnownSid(WinWorldSid, nullptr, world, &bytes)) {
            const auto error = GetLastError();
            LocalFree(m_descriptor);
            m_descriptor = nullptr;
            throw std::system_error((int)error, std::system_category(), "create fixture trustee");
        }
        EXPLICIT_ACCESSW denied{};
        denied.grfAccessPermissions = rights;
        denied.grfAccessMode        = DENY_ACCESS;
        denied.grfInheritance      = NO_INHERITANCE;
        BuildTrusteeWithSidW(&denied.Trustee, world);
        PACL replacement = nullptr;
        auto error = SetEntriesInAclW(1, &denied, m_original_acl, &replacement);
        if (error == ERROR_SUCCESS) {
            error = SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, replacement, nullptr);
        }
        if (replacement) {
            LocalFree(replacement);
        }
        if (error != ERROR_SUCCESS) {
            LocalFree(m_descriptor);
            m_descriptor = nullptr;
            throw std::system_error((int)error, std::system_category(), "deny fixture access");
        }
    }

    Access_denial(const Access_denial&) = delete;
    Access_denial& operator=(const Access_denial&) = delete;

    ~Access_denial()
    {
        if (m_descriptor) {
            const auto error = SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION |
                    (m_protected ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                nullptr, nullptr, m_original_acl, nullptr);
            if (error != ERROR_SUCCESS) {
                restoration_failed = true;
                std::cerr << "FAIL: restore fixture ACL: " << error << '\n';
            }
            LocalFree(m_descriptor);
        }
    }

private:
    std::wstring m_path;
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    PACL m_original_acl = nullptr;
    bool m_protected = false;
};

} // namespace filesystem_test
#endif
