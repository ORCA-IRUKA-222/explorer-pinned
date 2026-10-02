#include "common.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <functional>

#include "agent.h"
#include "console.h"
#include "pinstore.h"
#include "resource.h"
#include "setup.h"
#include "util.h"

using namespace ep;

namespace {

enum ExitCode { kOk = 0, kError = 1, kUsage = 2 };

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

void ShowInfo(const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
    if (HasConsoleOutput())
        ConsoleWrite(text + L"\n");
    else
        MessageBoxW(nullptr, text.c_str(), LoadStr(IDS_APP_NAME).c_str(), MB_OK | icon);
}

void NotifyAgent(UINT msg, WPARAM wp = 0) {
    if (HWND agent = FindAgentWindow()) PostMessageW(agent, msg, wp, 0);
}

// Waits until the agent in this session has exited.
void StopAgent() {
    HWND agent = FindAgentWindow();
    if (!agent) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(agent, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(agent, kMsgExit, 1, 0);
    if (process) {
        WaitForSingleObject(process, 15000);
        CloseHandle(process);
    }
}

int ChangePins(const std::vector<std::wstring>& paths, const std::wstring& mode) {
    if (paths.empty()) return kUsage;
    int failures = 0;
    for (const auto& raw : paths) {
        std::wstring path = NormalizePath(raw);
        bool pin = mode == L"pin" || (mode == L"toggle" && !PinStore::IsPinned(path));
        // A missing item can still be unpinned.
        if (path.empty() || (pin && !PathExists(path))) {
            ConsoleWrite(L"not found: " + raw + L"\n");
            failures++;
            continue;
        }
        if (pin)
            PinStore::Add(path);
        else
            PinStore::Remove(path);
        ConsoleWrite((pin ? L"pinned: " : L"unpinned: ") + path + L"\n");
    }
    UpdateContextMenu(PinStore::LoadAll());
    EnsureAgentRunning();
    return failures ? kError : kOk;
}

int ListPins() {
    for (const auto& p : PinStore::LoadAll()) ConsoleWrite(p + (PathExists(p) ? L"\n" : L"  (missing)\n"));
    return kOk;
}

std::wstring SchemaLabel() {
    std::wstring label;
    IPropertyDescription* desc = nullptr;
    if (SUCCEEDED(PSGetPropertyDescription(kPinStateKeys[0], IID_PPV_ARGS(&desc)))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(desc->GetDisplayName(&name)) && name) {
            label = name;
            CoTaskMemFree(name);
        }
        desc->Release();
    }
    return label;
}

int Status() {
    std::wstring s;
    s += L"schema: " + std::wstring(IsSchemaRegistered() ? L"registered (" + SchemaLabel() + L")" : L"not registered") + L"\n";
    s += L"menu: " + std::wstring(IsContextMenuRegistered() ? L"registered" : L"not registered") + L"\n";
    s += L"startup: " + std::wstring(IsStartupEnabled() ? L"on" : L"off") + L"\n";
    s += L"agent: " + std::wstring(FindAgentWindow() ? L"running" : L"not running") + L"\n";
    s += L"pins: " + std::to_wstring(PinStore::LoadAll().size()) + L"\n";
    ConsoleWrite(s);
    return kOk;
}

int RegisterSchemaCommand() {
    HRESULT hr = RegisterSchema();
    if (FAILED(hr)) {
        wchar_t msg[64];
        swprintf_s(msg, L"register-schema failed: 0x%08lX\n", (unsigned long)hr);
        ConsoleWrite(msg);
    }
    return SUCCEEDED(hr) ? kOk : kError;
}

int UnregisterSchemaCommand() {
    UnregisterSchema();
    return kOk;
}

// Registers the property schema, elevating when necessary.
bool EnsureSchema() {
    if (IsSchemaRegistered()) return true;
    if (IsElevated()) return SUCCEEDED(RegisterSchema());
    // The elevated process may run as another account; pass on this user's language.
    std::wstring lang = PRIMARYLANGID(UiLanguage()) == LANG_JAPANESE ? L"ja" : L"en";
    if (RunElevated(L"register-schema --lang " + lang) != 0) return false;
    PSRefreshPropertySchema();
    return IsSchemaRegistered();
}

// `language` is the value of --lang, if given: it becomes the user's language setting so that
// the menu, the group label and the notification area menu all use the installer's language.
int Setup(bool startup, bool startAgent, bool quiet, const std::wstring& language) {
    if (!language.empty())
        RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Language", PRIMARYLANGID(UiLanguage()) == LANG_JAPANESE ? L"ja" : L"en");
    bool schemaOk = EnsureSchema();
    UpdateContextMenu(PinStore::LoadAll());
    if (startup) SetStartupEnabled(true);
    if (startAgent) EnsureAgentRunning();
    if (!quiet) ShowInfo(LoadStr(schemaOk ? IDS_MSG_SETUP_DONE : IDS_MSG_SCHEMA_FAILED),
                         schemaOk ? MB_ICONINFORMATION : MB_ICONWARNING);
    return schemaOk ? kOk : kError;
}

int Uninstall(bool keepSchema, bool quiet) {
    StopAgent();              // restores the original grouping of open windows
    RestoreSavedGroupings();  // and of folders that are not open
    RemoveContextMenu();
    SetStartupEnabled(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegRoot);
    if (!keepSchema && IsSchemaRegistered()) {
        if (IsElevated())
            UnregisterSchema();
        else
            RunElevated(L"unregister-schema");
    }
    if (!quiet) ShowInfo(LoadStr(IDS_MSG_UNINSTALLED));
    return kOk;
}

// Double-click on the executable: set up on first run, then run in the background.
int DefaultStart() {
    if (FindAgentWindow()) {
        ShowInfo(LoadStr(IDS_MSG_ALREADY_RUNNING));
        return kOk;
    }
    if (!IsSchemaRegistered() || !IsContextMenuRegistered()) {
        if (MessageBoxW(nullptr, LoadStr(IDS_MSG_SETUP_PROMPT).c_str(), LoadStr(IDS_APP_NAME).c_str(),
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return kOk;
        if (Setup(/*startup=*/true, /*startAgent=*/false, /*quiet=*/false, /*language=*/L"") != kOk) return kError;
    }
    return RunAgent();
}

int Dispatch(const std::wstring& command, const std::vector<std::wstring>& rest,
             const std::function<bool(const wchar_t*)>& hasFlag, const std::wstring& language) {
    if (command.empty()) return DefaultStart();
    if (command == L"pin" || command == L"unpin" || command == L"toggle") return ChangePins(rest, command);
    if (command == L"list") return ListPins();
    if (command == L"status") return Status();
    if (command == L"reapply") {
        NotifyAgent(kMsgReapply);
        return kOk;
    }
    if (command == L"exit") {
        StopAgent();
        RestoreSavedGroupings();
        return kOk;
    }
    if (command == L"restore-groupings") {
        RestoreSavedGroupings();
        return kOk;
    }
    if (command == L"setup")
        return Setup(!hasFlag(L"--no-startup"), !hasFlag(L"--no-agent"), hasFlag(L"--quiet"), language);
    if (command == L"uninstall") return Uninstall(hasFlag(L"--keep-schema"), hasFlag(L"--quiet"));
    if (command == L"register-schema") return RegisterSchemaCommand();
    if (command == L"unregister-schema") return UnregisterSchemaCommand();

    ShowInfo(LoadStr(IDS_MSG_USAGE));
    return (command == L"help" || command == L"--help" || command == L"/?" || command == L"-h") ? kOk : kUsage;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    std::wstring command = args.empty() ? L"" : Lower(args[0]);
    std::vector<std::wstring> rest = args.empty() ? args : std::vector<std::wstring>(args.begin() + 1, args.end());
    auto hasFlag = [&](const wchar_t* flag) {
        return std::any_of(rest.begin(), rest.end(), [&](const std::wstring& a) { return Lower(a) == flag; });
    };
    std::wstring language;
    for (size_t i = 0; i + 1 < rest.size(); i++)
        if (Lower(rest[i]) == L"--lang") language = Lower(rest[i + 1]);
    if (!language.empty()) SetUiLanguage(language);

    if (command == L"agent") return RunAgent();  // initializes OLE itself

    // The property system and shell APIs used below need COM.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = Dispatch(command, rest, hasFlag, language);
    if (SUCCEEDED(hr)) CoUninitialize();
    return code;
}
