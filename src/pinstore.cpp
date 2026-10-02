#include "pinstore.h"

#include "util.h"

namespace ep {

std::vector<std::wstring> PinStore::LoadAll() {
    std::vector<std::wstring> result;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPins, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return result;
    DWORD maxName = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxName, nullptr, nullptr,
                     nullptr);
    std::wstring name(maxName + 1, L'\0');
    for (DWORD i = 0;; i++) {
        DWORD len = (DWORD)name.size();
        LONG r = RegEnumValueW(key, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r == ERROR_MORE_DATA) {
            name.resize(name.size() * 2);
            i--;
            continue;
        }
        if (r != ERROR_SUCCESS) break;
        if (len) result.emplace_back(name.c_str(), len);
    }
    RegCloseKey(key);
    return result;
}

bool PinStore::IsPinned(const std::wstring& path) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPins, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    LONG r = RegQueryValueExW(key, path.c_str(), nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool PinStore::Add(const std::wstring& path) {
    if (path.empty() || IsPinned(path)) return false;
    // The data holds the pin time; it is informational only.
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    return RegWriteString(HKEY_CURRENT_USER, kRegPins, path.c_str(), stamp);
}

bool PinStore::Remove(const std::wstring& path) { return RegDeleteValueIn(HKEY_CURRENT_USER, kRegPins, path.c_str()); }

int PinStore::RemoveMissing() {
    int removed = 0;
    for (const auto& p : LoadAll()) {
        if (!PathExists(p) && Remove(p)) removed++;
    }
    return removed;
}

std::vector<std::wstring> PinStore::NamesInFolder(const std::vector<std::wstring>& pins, const std::wstring& folder) {
    std::vector<std::wstring> names;
    for (const auto& p : pins) {
        if (PathEqualsI(ParentPath(p), folder)) names.push_back(FileNamePart(p));
    }
    return names;
}

}  // namespace ep
