#include "dialogs.h"

#include <knownfolders.h>
#include <shlobj.h>

#include "util.h"

namespace ep {
namespace {

constexpr wchar_t kClsid[] = L"{432E90E6-6BCF-44FE-9F87-8BA191F04870}";
constexpr wchar_t kDescription[] = L"Explorer Pinned (file dialogs)";
constexpr wchar_t kClsidRoot[] = L"Software\\Classes\\CLSID";
constexpr wchar_t kApproved[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
constexpr wchar_t kOverlayRoot[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";
// Windows loads the overlay handlers in the order of these names and uses only the first
// 15 overlays. The leading spaces get this handler loaded before the others; it takes no
// overlay slot (it has no overlay image), so the overlays of other programs are not affected.
constexpr wchar_t kOverlayName[] = L"    ExplorerPinned";

constexpr wchar_t kBags[] = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags";

bool WriteMachineString(REGSAM view, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void DeleteMachineKey(REGSAM view, const std::wstring& parent, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent.c_str(), 0, KEY_ALL_ACCESS | view, &k) != ERROR_SUCCESS) return;
    RegDeleteTreeW(k, name);
    RegCloseKey(k);
}

bool StartsWithFolder(const std::wstring& path, REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) return false;
    std::wstring prefix = NormalizePath(folder) + L"\\";
    CoTaskMemFree(folder);
    std::wstring p = NormalizePath(path);
    return p.size() > prefix.size() && _wcsnicmp(p.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool IsPinFormatId(const wchar_t* value) {
    return _wcsicmp(value, kPinStateFormatId) == 0 || _wcsicmp(value, kLegacyPinStateFormatId) == 0;
}

// Bags\<n>\<ComDlg|ComDlgLegacy|Shell>\<folder type>: values such as GroupByKey:FMTID.
int ResetBags(HKEY parent, const std::wstring& path, int depth) {
    HKEY k;
    if (RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return 0;
    int count = 0;
    wchar_t fmtid[64] = L"";
    DWORD size = sizeof(fmtid) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(k, L"GroupByKey:FMTID", nullptr, &type, reinterpret_cast<BYTE*>(fmtid), &size) ==
            ERROR_SUCCESS &&
        type == REG_SZ && IsPinFormatId(fmtid)) {
        static const wchar_t kNone[] = L"{00000000-0000-0000-0000-000000000000}";
        DWORD zero = 0;
        RegSetValueExW(k, L"GroupByKey:FMTID", 0, REG_SZ, reinterpret_cast<const BYTE*>(kNone), sizeof(kNone));
        RegSetValueExW(k, L"GroupByKey:PID", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        count++;
    }
    if (depth < 3) {
        std::vector<std::wstring> children;
        wchar_t name[256];
        for (DWORD i = 0;; i++) {
            DWORD len = ARRAYSIZE(name);
            if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            children.push_back(name);
        }
        for (const auto& child : children) count += ResetBags(k, child, depth + 1);
    }
    RegCloseKey(k);
    return count;
}

}  // namespace

bool RegisterDialogExtension(REGSAM view, const std::wstring& dllPath) {
    std::wstring clsidKey = std::wstring(kClsidRoot) + L"\\" + kClsid;
    std::wstring overlayKey = std::wstring(kOverlayRoot) + L"\\" + kOverlayName;
    bool ok = WriteMachineString(view, clsidKey, nullptr, kDescription) &&
              WriteMachineString(view, clsidKey + L"\\InprocServer32", nullptr, dllPath) &&
              WriteMachineString(view, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              WriteMachineString(view, overlayKey, nullptr, kClsid);
    WriteMachineString(view, kApproved, kClsid, kDescription);
    if (!ok) UnregisterDialogExtension(view);
    return ok;
}

void UnregisterDialogExtension(REGSAM view) {
    DeleteMachineKey(view, kOverlayRoot, kOverlayName);
    DeleteMachineKey(view, kClsidRoot, kClsid);
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kApproved, 0, KEY_SET_VALUE | view, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kClsid);
        RegCloseKey(k);
    }
}

std::wstring RegisteredDialogExtension(REGSAM view) {
    std::wstring key = std::wstring(kClsidRoot) + L"\\" + kClsid + L"\\InprocServer32";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
    wchar_t path[MAX_PATH] = L"";
    DWORD size = sizeof(path) - sizeof(wchar_t), type = 0;
    bool ok = RegQueryValueExW(k, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(path), &size) == ERROR_SUCCESS &&
              type == REG_SZ;
    RegCloseKey(k);
    // Without the overlay entry, Windows does not load it.
    std::wstring overlayKey = std::wstring(kOverlayRoot) + L"\\" + kOverlayName;
    if (!ok || RegOpenKeyExW(HKEY_LOCAL_MACHINE, overlayKey.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
    RegCloseKey(k);
    return path;
}

bool IsProtectedLocation(const std::wstring& path) {
    return StartsWithFolder(path, FOLDERID_ProgramFiles) || StartsWithFolder(path, FOLDERID_ProgramFilesX86);
}

bool DialogsSettingOn() {
    std::wstring value;
    return !(RegReadString(HKEY_CURRENT_USER, kRegRoot, L"Dialogs", &value) && value == L"0");
}

void SetDialogsSetting(bool on) { RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Dialogs", on ? L"1" : L"0"); }

int ResetSavedPinnedGroupings() { return ResetBags(HKEY_CURRENT_USER, kBags, 0); }

}  // namespace ep
