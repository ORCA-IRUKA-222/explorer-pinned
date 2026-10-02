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

std::wstring SchemaPath() { return ExeDirectory() + L"\\ExplorerPinned.propdesc"; }

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
std::wstring SchemaXml() {
    std::wstring label = XmlEscape(LoadStr(IDS_PROP_LABEL));
    std::wstring pinned = XmlEscape(LoadStr(IDS_PROP_PINNED));
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"utf-16\"?>\r\n"
        L"<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
        L"  <propertyDescriptionList publisher=\"ExplorerPinned\" product=\"ExplorerPinned\">\r\n";
    for (size_t i = 0; i < std::size(kPinStateKeys); i++) {
        xml += L"    <propertyDescription name=\"" + std::wstring(kPinStateNames[i]) + L"\" formatID=\"" +
               kPinStateFormatId + L"\" propID=\"" + std::to_wstring(kPinStateKeys[i].pid) + L"\">\r\n";
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

HRESULT RegisterSchema() {
    std::wstring path = SchemaPath();
    if (!WriteUtf16File(path, SchemaXml())) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = PSRegisterPropertySchema(path.c_str());
    if (SUCCEEDED(hr)) PSRefreshPropertySchema();
    LogLine(L"RegisterSchema(%s) hr=0x%08x", path.c_str(), hr);
    return hr;
}

HRESULT UnregisterSchema() {
    std::wstring path = SchemaPath();
    HRESULT hr = PSUnregisterPropertySchema(path.c_str());
    if (SUCCEEDED(hr)) PSRefreshPropertySchema();
    DeleteFileW(path.c_str());
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
