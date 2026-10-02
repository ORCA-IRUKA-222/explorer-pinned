#include "util.h"

#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>

namespace ep {

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring p = path;
    // Strip surrounding quotes that may come from a command line.
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);

    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n) {
        std::wstring full(n, L'\0');
        n = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
        full.resize(n);
        p = full;
    }
    n = GetLongPathNameW(p.c_str(), nullptr, 0);
    if (n) {
        std::wstring lp(n, L'\0');
        n = GetLongPathNameW(p.c_str(), lp.data(), n);
        if (n) {
            lp.resize(n);
            p = lp;
        }
    }
    // Remove trailing separators but keep "C:\".
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

std::wstring ParentPath(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring();
    if (pos == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);  // "C:\"
    return path.substr(0, pos);
}

std::wstring FileNamePart(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool PathEqualsI(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDirectory() { return ParentPath(ExePath()); }

std::wstring LoadStr(UINT id) {
    const wchar_t* p = nullptr;
    int n = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&p), 0);
    return n > 0 ? std::wstring(p, n) : std::wstring();
}

std::wstring FormatStr(UINT id, const std::wstring& arg) {
    std::wstring fmt = LoadStr(id);
    size_t pos = fmt.find(L"%s");
    if (pos != std::wstring::npos) fmt.replace(pos, 2, arg);
    return fmt;
}

std::wstring IndirectStr(UINT id) { return L"@" + ExePath() + L",-" + std::to_wstring(id); }

bool RegWriteString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool RegReadString(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* value) {
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    size = (DWORD)(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS) return false;
    buf.resize(wcslen(buf.c_str()));
    *value = buf;
    return true;
}

bool RegDeleteValueIn(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LONG r = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// Diagnostics go to OutputDebugString always, and to
// %LOCALAPPDATA%\ExplorerPinned\agent.log when HKCU\Software\ExplorerPinned\Log = "1".
void LogLine(const wchar_t* fmt, ...) {
    static int enabled = -1;
    static std::wstring logPath;
    if (enabled < 0) {
        std::wstring v;
        enabled = RegReadString(HKEY_CURRENT_USER, kRegRoot, L"Log", &v) && v == L"1";
        if (enabled) {
            PWSTR dir = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir))) {
                logPath = std::wstring(dir) + L"\\ExplorerPinned";
                CreateDirectoryW(logPath.c_str(), nullptr);
                logPath += L"\\agent.log";
                CoTaskMemFree(dir);
            }
        }
    }
    wchar_t buf[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int prefix = swprintf_s(buf, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf + prefix, _countof(buf) - prefix, _TRUNCATE, fmt, ap);
    va_end(ap);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    if (enabled > 0 && !logPath.empty()) {
        HANDLE h = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char out[4096];
            int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out) - 2, nullptr, nullptr);
            if (n > 0) {
                out[n - 1] = '\r';
                out[n] = '\n';
                DWORD w;
                WriteFile(h, out, n + 1, &w, nullptr);
            }
            CloseHandle(h);
        }
    }
}

}  // namespace ep
