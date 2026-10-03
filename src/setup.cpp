#include "setup.h"

#include <shellapi.h>
#include <shlobj.h>

#include "resource.h"
#include "util.h"

namespace ep {
namespace {

constexpr wchar_t kVerbPin[] = L"ExplorerPinned.Pin";
constexpr wchar_t kVerbUnpin[] = L"ExplorerPinned.Unpin";
constexpr const wchar_t* kClasses[] = {L"*", L"Directory"};

std::wstring VerbKey(const wchar_t* cls, const wchar_t* verb) {
    return std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\" + verb;
}

// AQS condition matching exactly the given paths, e.g.
// System.ParsingPath:="C:\a.txt" OR System.ParsingPath:="C:\b"
// Windows paths cannot contain double quotes, so no escaping is needed.
std::wstring PathCondition(const std::vector<std::wstring>& pins) {
    std::wstring cond;
    for (const auto& p : pins) {
        if (!cond.empty()) cond += L" OR ";
        cond += L"System.ParsingPath:=\"" + p + L"\"";
    }
    return cond;
}

void WriteVerb(const wchar_t* cls, const wchar_t* verb, UINT labelId, const std::wstring& command,
               const std::wstring& appliesTo) {
    std::wstring key = VerbKey(cls, verb);
    std::wstring exe = ExePath();
    RegWriteString(HKEY_CURRENT_USER, key, L"MUIVerb", LoadStr(labelId));
    RegWriteString(HKEY_CURRENT_USER, key, L"Icon", L"\"" + exe + L"\",0");
    if (appliesTo.empty())
        RegDeleteValueIn(HKEY_CURRENT_USER, key, L"AppliesTo");
    else
        RegWriteString(HKEY_CURRENT_USER, key, L"AppliesTo", appliesTo);
    RegWriteString(HKEY_CURRENT_USER, key + L"\\command", nullptr, L"\"" + exe + L"\" " + command + L" \"%1\"");
}

std::wstring SchemaDirectory() {
    std::wstring result;
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &dir))) {
        result = std::wstring(dir) + L"\\" + kSchemaDirName;
        CoTaskMemFree(dir);
    }
    return result;
}

constexpr wchar_t kSchemaListKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertySchema";

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': r += L"&amp;"; break;
            case L'<': r += L"&lt;"; break;
            case L'>': r += L"&gt;"; break;
            case L'"': r += L"&quot;"; break;
            default: r += c;
        }
    }
    return r;
}

// The labels are written as text in the current UI language: the schema is machine-wide
// and Explorer shows these strings as the group name.
std::wstring SchemaXml(const wchar_t* formatId, const PROPERTYKEY (&keys)[2], const wchar_t* const (&names)[2]) {
    std::wstring label = XmlEscape(LoadStr(IDS_PROP_LABEL));
    std::wstring pinned = XmlEscape(LoadStr(IDS_PROP_PINNED));
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"utf-16\"?>\r\n"
        L"<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
        L"  <propertyDescriptionList publisher=\"ExplorerPinned\" product=\"ExplorerPinned\">\r\n";
    for (size_t i = 0; i < 2; i++) {
        xml += L"    <propertyDescription name=\"" + std::wstring(names[i]) + L"\" formatID=\"" + formatId +
               L"\" propID=\"" + std::to_wstring(keys[i].pid) + L"\">\r\n";
        xml +=
            L"      <searchInfo inInvertedIndex=\"false\" isColumn=\"false\"/>\r\n"
            L"      <typeInfo type=\"UInt32\" isInnate=\"true\" isViewable=\"true\" groupingRange=\"Enumerated\"/>\r\n"
            L"      <labelInfo label=\"" + label + L"\"/>\r\n"
            L"      <displayInfo displayType=\"Enumerated\" defaultColumnWidth=\"12\">\r\n"
            L"        <enumeratedList>\r\n"
            L"          <enum name=\"Pinned\" value=\"" + std::to_wstring(kPinnedValue) + L"\" text=\"" + pinned + L"\"/>\r\n"
            L"        </enumeratedList>\r\n"
            L"      </displayInfo>\r\n"
            L"    </propertyDescription>\r\n";
    }
    xml += L"  </propertyDescriptionList>\r\n</schema>\r\n";
    return xml;
}

bool WriteUtf16File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const WCHAR bom = 0xFEFF;
    BOOL ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) &&
              WriteFile(h, text.c_str(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

}  // namespace

void UpdateContextMenu(const std::vector<std::wstring>& pins) {
    std::wstring cond = PathCondition(pins);
    for (const wchar_t* cls : kClasses) {
        UINT pinLabel = wcscmp(cls, L"Directory") == 0 ? IDS_MENU_PIN_FOLDER : IDS_MENU_PIN_FILE;
        WriteVerb(cls, kVerbPin, pinLabel, L"pin", cond.empty() ? std::wstring() : L"NOT (" + cond + L")");
        if (cond.empty())
            RegDeleteTreeW(HKEY_CURRENT_USER, VerbKey(cls, kVerbUnpin).c_str());
        else
            WriteVerb(cls, kVerbUnpin, IDS_MENU_UNPIN, L"unpin", cond);
    }
}

void RemoveContextMenu() {
    for (const wchar_t* cls : kClasses) {
        RegDeleteTreeW(HKEY_CURRENT_USER, VerbKey(cls, kVerbPin).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, VerbKey(cls, kVerbUnpin).c_str());
    }
}

bool IsContextMenuRegistered() {
    std::wstring command;
    if (!RegReadString(HKEY_CURRENT_USER, VerbKey(L"*", kVerbPin) + L"\\command", nullptr, &command)) return false;
    // Re-register when the executable has moved.
    return command.find(ExePath()) != std::wstring::npos;
}

bool IsStartupEnabled() {
    std::wstring value;
    return RegReadString(HKEY_CURRENT_USER, kRegRun, kRunValueName, &value);
}

void SetStartupEnabled(bool enabled) {
    if (enabled)
        RegWriteString(HKEY_CURRENT_USER, kRegRun, kRunValueName, L"\"" + ExePath() + L"\" agent");
    else
        RegDeleteValueIn(HKEY_CURRENT_USER, kRegRun, kRunValueName);
}

bool IsSchemaRegistered() {
    IPropertyDescription* desc = nullptr;
    for (const auto& key : kPinStateKeys) {
        if (FAILED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc)))) return false;
        desc->Release();
    }
    return true;
}

std::wstring SchemaPath() { return SchemaDirectory() + L"\\" + kSchemaFileName; }

// Schema registrations of this program: the current file and ExplorerPinned.propdesc of
// versions 1.0.0 and 1.0.1, wherever they were registered from. Windows identifies a schema
// by its file name (the "URI" value).
std::vector<SchemaRegistration> SchemaRegistrations() {
    std::vector<SchemaRegistration> result;
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSchemaListKey, 0, KEY_READ, &root) != ERROR_SUCCESS) return result;
    wchar_t sub[256];
    for (DWORD i = 0;; i++) {
        DWORD len = ARRAYSIZE(sub);
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring path, uri;
        RegReadString(root, sub, nullptr, &path);
        RegReadString(root, sub, L"URI", &uri);
        std::wstring file = FileNamePart(path);
        bool legacy = PathEqualsI(uri, kLegacySchemaFileName) || PathEqualsI(file, kLegacySchemaFileName);
        bool current = PathEqualsI(uri, kSchemaFileName) || PathEqualsI(file, kSchemaFileName);
        if (legacy || current) result.push_back({path, legacy, PathExists(path)});
    }
    RegCloseKey(root);
    return result;
}

namespace {

// Removes every registration of `path`. Windows reports success for a file that no longer
// exists but keeps its registration (which then keeps the descriptions from loading), so a
// missing file is written again for the duration of the call.
void RemoveRegistration(const SchemaRegistration& r) {
    std::vector<std::wstring> createdDirs;
    bool createdFile = false;
    if (!r.exists && !r.path.empty()) {
        for (std::wstring dir = ParentPath(r.path); !dir.empty() && !PathExists(dir); dir = ParentPath(dir)) {
            createdDirs.push_back(dir);
            if (dir.size() <= 3) break;
        }
        for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) CreateDirectoryW(it->c_str(), nullptr);
        createdFile = WriteUtf16File(r.path, r.legacy ? SchemaXml(kLegacyPinStateFormatId, kLegacyPinStateKeys, kLegacyPinStateNames)
                                                      : SchemaXml(kPinStateFormatId, kPinStateKeys, kPinStateNames));
    }
    // Windows also reports success when the file is not registered; stop once it is gone,
    // since every call makes running programs reload their property descriptions.
    auto registered = [&] {
        for (const auto& other : SchemaRegistrations())
            if (PathEqualsI(other.path, r.path)) return true;
        return false;
    };
    int removed = 0;
    while (removed < 8 && registered() && SUCCEEDED(PSUnregisterPropertySchema(r.path.c_str()))) removed++;
    if (createdFile) DeleteFileW(r.path.c_str());
    for (const auto& dir : createdDirs) RemoveDirectoryW(dir.c_str());
    LogLine(L"unregister schema %s (%s, %s): %d", r.path.c_str(), r.legacy ? L"old" : L"current",
            r.exists ? L"present" : L"missing", removed);
}

void LogRegistrations(const wchar_t* when) {
    auto list = SchemaRegistrations();
    LogLine(L"schema registrations %s: %zu", when, list.size());
    for (const auto& r : list)
        LogLine(L"  %s (%s, %s)", r.path.c_str(), r.legacy ? L"old" : L"current", r.exists ? L"present" : L"missing");
}

}  // namespace

HRESULT RegisterSchema() {
    // Start from a clean slate: a second registration of a file with the same name (from
    // another folder, or one whose file is gone) makes the registration fail partly.
    LogRegistrations(L"before");
    for (const auto& r : SchemaRegistrations()) RemoveRegistration(r);
    // A copy of version 1.0.x installed in this folder left its schema file here.
    DeleteFileW((ExeDirectory() + L"\\" + kLegacySchemaFileName).c_str());
    PSRefreshPropertySchema();

    std::wstring dir = SchemaDirectory(), path = SchemaPath();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (!WriteUtf16File(path, SchemaXml(kPinStateFormatId, kPinStateKeys, kPinStateNames)))
        return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = PSRegisterPropertySchema(path.c_str());
    PSRefreshPropertySchema();
    LogLine(L"RegisterSchema(%s) hr=0x%08x", path.c_str(), hr);
    if (hr != S_OK) LogRegistrations(L"after");
    return hr;
}

HRESULT UnregisterSchema() {
    for (const auto& r : SchemaRegistrations()) RemoveRegistration(r);
    HRESULT hr = PSRefreshPropertySchema();
    DeleteFileW(SchemaPath().c_str());
    RemoveDirectoryW(SchemaDirectory().c_str());
    DeleteFileW((ExeDirectory() + L"\\" + kLegacySchemaFileName).c_str());
    return hr;
}

int RunElevated(const std::wstring& args) {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

}  // namespace ep
