// In-process part for file dialogs (the Open, Save and upload dialogs of any program).
//
// The agent reaches Explorer windows from outside through IShellWindows, but a file dialog
// lives inside the program that shows it and cannot be reached from another process.
// Windows loads icon overlay handlers into every process that shows a folder view, so this
// DLL registers one (it never draws an overlay). Loaded into a program other than Explorer,
// it applies the pinned grouping to that program's file dialogs from inside.

#include "common.h"

#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <map>
#include <memory>
#include <set>

#include "comutil.h"
#include "pinstore.h"
#include "dialogs.h"
#include "toggle.h"
#include "util.h"

namespace ep {
namespace {

// {432E90E6-6BCF-44FE-9F87-8BA191F04870}
const CLSID kOverlayClsid = {0x432e90e6, 0x6bcf, 0x44fe, {0x9f, 0x87, 0x8b, 0xa1, 0x91, 0xf0, 0x48, 0x70}};

constexpr UINT kGetShellBrowser = WM_USER + 7;  // WM_GETISHELLBROWSER, answered by shell browser windows
// Dialogs are found through window events, and by a scan every second (for example a dialog
// that was being set up while this DLL was loaded).
constexpr DWORD kScanMs = 1000;
// A new folder is grouped once Explorer reports it listed it (EnumDone), once its item count
// stops changing, or at the latest after this.
constexpr DWORD kNewViewDelayMs = 600;
constexpr UINT kRecheckMs = 80;
constexpr UINT_PTR kTimerId = 0xE9A1;  // on the dialog window; swallowed by the message hook
constexpr int kMaxAttempts = 4;

HMODULE g_module = nullptr;
LONG g_objects = 0;
UINT g_tickMessage = 0;

// ---------------------------------------------------------------- grouping

struct LessI {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
    }
};
using NameSet = std::set<std::wstring, LessI>;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
HRESULT SetViewValue(IFolderView2* view, PCUITEMID_CHILD child, const PROPERTYKEY& key, const PROPVARIANT& value) {
    return view->SetViewProperty(child, key, value);
}
HRESULT GetViewValue(IFolderView2* view, PCUITEMID_CHILD child, const PROPERTYKEY& key, PROPVARIANT* value) {
    return view->GetViewProperty(child, key, value);
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

int KeyIndex(const PROPERTYKEY& key) {
    for (int i = 0; i < 2; i++)
        if (IsEqualPropertyKey(key, kPinStateKeys[i])) return i;
    return -1;
}

bool IsPinKey(const PROPERTYKEY& key) {
    return KeyIndex(key) >= 0 || IsEqualPropertyKey(key, kLegacyPinStateKeys[0]) ||
           IsEqualPropertyKey(key, kLegacyPinStateKeys[1]);
}

// The grouping a folder had in dialogs before it was grouped by pins; dialogs save their view
// state per folder, so a dialog opened later may already show the pinned grouping.
void SavePreviousGrouping(const std::wstring& folder, const PROPERTYKEY& key, BOOL ascending) {
    std::wstring existing;
    if (RegReadString(HKEY_CURRENT_USER, kRegPreviousDialogGroupBy, folder.c_str(), &existing)) return;
    wchar_t guid[64];
    StringFromGUID2(key.fmtid, guid, ARRAYSIZE(guid));
    RegWriteString(HKEY_CURRENT_USER, kRegPreviousDialogGroupBy, folder.c_str(),
                   std::wstring(guid) + L"," + std::to_wstring(key.pid) + L"," + (ascending ? L"1" : L"0"));
}

void ReadPreviousGrouping(const std::wstring& folder, PROPERTYKEY* key, BOOL* ascending) {
    *key = PROPERTYKEY{GUID_NULL, 0};
    *ascending = TRUE;
    std::wstring value;
    if (!RegReadString(HKEY_CURRENT_USER, kRegPreviousDialogGroupBy, folder.c_str(), &value)) return;
    size_t c1 = value.find(L',');
    size_t c2 = value.find(L',', c1 + 1);
    GUID g = GUID_NULL;
    if (c1 == std::wstring::npos || c2 == std::wstring::npos || FAILED(CLSIDFromString(value.substr(0, c1).c_str(), &g)))
        return;
    PROPERTYKEY k = {g, (DWORD)wcstoul(value.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10)};
    if (IsPinKey(k)) return;
    *key = k;
    *ascending = value.substr(c2 + 1) != L"0";
}

PITEMID_CHILD ParseChild(IShellFolder* folder, const std::wstring& name) {
    PIDLIST_RELATIVE pidl = nullptr;
    std::wstring buf = name;
    if (FAILED(folder->ParseDisplayName(nullptr, nullptr, buf.data(), nullptr, &pidl, nullptr))) return nullptr;
    if (!ILIsChild(pidl)) {
        CoTaskMemFree(pidl);
        return nullptr;
    }
    return static_cast<PITEMID_CHILD>(pidl);
}

// One file dialog; lives on the dialog's thread.
struct Dialog {
    ComPtr<IUnknown> viewIdentity;
    DWORD seen = 0;
    bool ready = false;     // the view has listed the folder
    bool enumDone = false;  // Explorer reported the end of the listing
    UINT lastCount = 0;
    std::wstring folderPath;
    NameSet applied[2];
    NameSet lastNames;
    bool grouped = false;  // the pinned grouping was applied to this view
    DWORD lastRegroup = 0;
    int attempts = 0;
    bool gaveUp = false;
};

thread_local std::map<HWND, Dialog>* t_dialogs = nullptr;
// Listing events of each dialog's current view (kept apart: Dialog is reset per view).
thread_local std::map<HWND, std::unique_ptr<EventConnection>>* t_viewEvents = nullptr;

// Looks at the dialog again after `ms` (a timer on the dialog's own thread).
void Recheck(HWND dialog, UINT ms) { SetTimer(dialog, kTimerId, ms, nullptr); }

size_t WriteKey(Dialog& d, IFolderView2* view, IShellFolder* folder, int k, const NameSet& names) {
    PROPVARIANT empty;
    PropVariantInit(&empty);
    for (const auto& name : d.applied[k]) {
        if (names.count(name)) continue;
        if (PITEMID_CHILD child = ParseChild(folder, name)) {
            SetViewValue(view, child, kPinStateKeys[k], empty);
            CoTaskMemFree(child);
        }
    }
    d.applied[k].clear();
    PROPVARIANT pinned;
    InitPropVariantFromUInt32(kPinnedValue, &pinned);
    for (const auto& name : names) {
        if (PITEMID_CHILD child = ParseChild(folder, name)) {
            if (SUCCEEDED(SetViewValue(view, child, kPinStateKeys[k], pinned))) d.applied[k].insert(name);
            CoTaskMemFree(child);
        }
    }
    return d.applied[k].size();
}

bool KeyMatches(Dialog& d, IFolderView2* view, IShellFolder* folder, int k, const NameSet& names) {
    for (const auto& name : d.applied[k])
        if (!names.count(name)) return false;
    for (const auto& name : names) {
        PITEMID_CHILD child = ParseChild(folder, name);
        if (!child) continue;
        PROPVARIANT value;
        PropVariantInit(&value);
        HRESULT hr = GetViewValue(view, child, kPinStateKeys[k], &value);
        bool ok = (FAILED(hr) && hr != TYPE_E_ELEMENTNOTFOUND) || (value.vt == VT_UI4 && value.ulVal == kPinnedValue);
        PropVariantClear(&value);
        CoTaskMemFree(child);
        if (!ok) return false;
    }
    return true;
}

ComPtr<IShellBrowser> ShellBrowserOf(HWND dialog, HWND defView) {
    // The window that answers WM_GETISHELLBROWSER differs between Windows versions:
    // try the folder view's ancestors up to the dialog.
    for (HWND w = GetParent(defView); w; w = (w == dialog) ? nullptr : GetParent(w)) {
        auto* sb = reinterpret_cast<IShellBrowser*>(SendMessageW(w, kGetShellBrowser, 0, 0));
        if (sb) {
            sb->AddRef();
            ComPtr<IShellBrowser> result;
            *result.Put() = sb;
            return result;
        }
    }
    return ComPtr<IShellBrowser>();
}

HWND FindDefView(HWND parent) {
    HWND found = nullptr;
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(child, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"SHELLDLL_DefView") == 0 &&
                IsWindowVisible(child)) {
                *reinterpret_cast<HWND*>(param) = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

void RestoreGrouping(Dialog& d, IFolderView2* view) {
    PROPERTYKEY key;
    BOOL ascending;
    ReadPreviousGrouping(d.folderPath, &key, &ascending);
    view->SetGroupBy(key, ascending);
    d.grouped = false;
    LogLine(L"dialog: restore grouping %s", d.folderPath.c_str());
}

ComPtr<IFolderView2> ViewOf(HWND dialog) {
    HWND defView = FindDefView(dialog);
    if (!defView) return ComPtr<IFolderView2>();
    ComPtr<IShellBrowser> browser = ShellBrowserOf(dialog, defView);
    ComPtr<IShellView> shellView;
    if (!browser || FAILED(browser->QueryActiveShellView(shellView.Put())) || !shellView) return ComPtr<IFolderView2>();
    return shellView.As<IFolderView2>();
}

// The dialog is about to close or to show another folder: put the original grouping back
// first, since the dialog saves its view state for the folder it leaves.
void BeforeLeave(HWND dialog) {
    if (!t_dialogs) return;
    auto it = t_dialogs->find(dialog);
    if (it == t_dialogs->end() || !it->second.grouped) return;
    ComPtr<IFolderView2> view = ViewOf(dialog);
    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    if (view && SUCCEEDED(view->GetGroupBy(&groupKey, &ascending)) && IsPinKey(groupKey)) RestoreGrouping(it->second, view.Get());
    // If the dialog stays open, group the view again.
    it->second.attempts = 0;
    it->second.lastRegroup = 0;
}

// Reports navigation inside a dialog (opening a subfolder, the address bar, the navigation
// pane), where no close command passes by.
class NavigationEvents : public IExplorerBrowserEvents {
public:
    explicit NavigationEvents(HWND dialog) : dialog_(dialog) {}
    virtual ~NavigationEvents() = default;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents) {
            *ppv = static_cast<IExplorerBrowserEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) override {
        BeforeLeave(dialog_);
        return S_OK;
    }
    STDMETHODIMP OnViewCreated(IShellView*) override { return S_OK; }
    STDMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE) override {
        PostMessageW(dialog_, g_tickMessage, 0, 0);
        return S_OK;
    }
    STDMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) override { return S_OK; }

private:
    HWND dialog_;
    LONG ref_ = 1;
};

struct Watched {
    ComPtr<IExplorerBrowser> browser;
    DWORD cookie = 0;
};
thread_local std::map<HWND, Watched>* t_watched = nullptr;

void WatchNavigation(HWND dialog) {
    if (!t_watched) t_watched = new std::map<HWND, Watched>();
    if (t_watched->count(dialog)) return;
    Watched& w = (*t_watched)[dialog];
    HWND defView = FindDefView(dialog);
    ComPtr<IShellBrowser> sb = defView ? ShellBrowserOf(dialog, defView) : ComPtr<IShellBrowser>();
    HRESULT hr = sb ? sb->QueryInterface(IID_PPV_ARGS(w.browser.Put())) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) {
        auto* events = new NavigationEvents(dialog);
        hr = w.browser->Advise(events, &w.cookie);
        events->Release();
    }
    // Without it (older style dialogs), only closing and typed paths are noticed.
    LogLine(L"dialog: navigation events 0x%08lX", (unsigned long)hr);
}

// The on/off button of each dialog, over its folder view while the folder has pinned items.
thread_local std::map<HWND, std::unique_ptr<ToggleButton>>* t_buttons = nullptr;

void UpdateButton(HWND dialog, bool show, bool on) {
    HWND defView = show && ToggleButtonEnabled() ? FindDefView(dialog) : nullptr;
    if (!defView) {
        if (t_buttons) {
            auto it = t_buttons->find(dialog);
            if (it != t_buttons->end()) it->second->Hide();
        }
        return;
    }
    if (!t_buttons) t_buttons = new std::map<HWND, std::unique_ptr<ToggleButton>>();
    auto& button = (*t_buttons)[dialog];
    if (!button) {
        button = std::make_unique<ToggleButton>();
        button->onClick = [dialog] {
            bool turnOn = !PinsEnabled();
            SetPinsEnabled(turnOn);
            LogLine(L"dialog: pinned group turned %s with the button", turnOn ? L"on" : L"off");
            PostMessageW(dialog, g_tickMessage, 0, 0);
        };
    }
    button->Show(dialog, defView, on);
}

void ForgetDialog(HWND dialog) {
    if (t_dialogs) t_dialogs->erase(dialog);
    if (t_viewEvents) t_viewEvents->erase(dialog);
    if (t_buttons) t_buttons->erase(dialog);
    if (!t_watched) return;
    auto it = t_watched->find(dialog);
    if (it == t_watched->end()) return;
    if (it->second.browser && it->second.cookie) it->second.browser->Unadvise(it->second.cookie);
    t_watched->erase(it);
}

bool IsCloseCommand(UINT message, WPARAM wp) {
    return (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) || message == WM_CLOSE ||
           (message == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE);
}

// Runs on the dialog's thread.
void ProcessDialog(HWND dialog) {
    if (!DialogsSettingOn()) {
        // Turned off in the notification area menu: leave the dialog as it was.
        BeforeLeave(dialog);
        ForgetDialog(dialog);
        return;
    }
    ComPtr<IFolderView2> view = ViewOf(dialog);
    if (!view) return;
    if (!t_dialogs) t_dialogs = new std::map<HWND, Dialog>();
    WatchNavigation(dialog);
    Dialog& d = (*t_dialogs)[dialog];
    DWORD now = GetTickCount();

    ComPtr<IUnknown> identity = Identity(view.Get());
    bool newView = identity.Get() != d.viewIdentity.Get();
    if (newView) {
        // A new folder: Explorer lists it and applies its saved view state first.
        d = Dialog();
        d.viewIdentity = identity;
        d.seen = now;
        ComPtr<IPersistFolder2> pf;
        if (SUCCEEDED(view->GetFolder(IID_PPV_ARGS(pf.Put())))) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pf->GetCurFolder(&pidl))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path))) {
                    d.folderPath = NormalizePath(path);
                    CoTaskMemFree(path);
                }
                CoTaskMemFree(pidl);
            }
        }
        // The view's automation object reports when the listing is done.
        if (!t_viewEvents) t_viewEvents = new std::map<HWND, std::unique_ptr<EventConnection>>();
        auto& events = (*t_viewEvents)[dialog];
        events = std::make_unique<EventConnection>();
        ComPtr<IShellView> sv = view.As<IShellView>();
        ComPtr<IDispatch> automation;
        if (sv && SUCCEEDED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(automation.Put()))) && automation) {
            events->Connect(automation.Get(), DIID_DShellFolderViewEvents, [dialog](DISPID id) {
                if (id != 201 || !t_dialogs) return;  // DISPID_FILELISTENUMDONE
                auto it = t_dialogs->find(dialog);
                if (it != t_dialogs->end()) it->second.enumDone = true;
                Recheck(dialog, 10);
            });
        }
        int count = 0;
        view->ItemCount(SVGIO_ALLVIEW, &count);
        d.lastCount = (UINT)count;
    }
    if (d.folderPath.empty()) {
        UpdateButton(dialog, false, true);
        return;
    }
    ComPtr<IShellFolder> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(folder.Put())))) return;
    NameSet names;
    for (const auto& n : PinStore::NamesInFolder(PinStore::LoadAll(), d.folderPath)) {
        if (PITEMID_CHILD child = ParseChild(folder.Get(), n)) {
            names.insert(n);
            CoTaskMemFree(child);
        }
    }
    bool enabled = PinsEnabled();
    UpdateButton(dialog, !names.empty(), enabled);
    if (newView) {
        Recheck(dialog, kRecheckMs);
        return;
    }
    if (!d.ready) {
        int count = 0;
        view->ItemCount(SVGIO_ALLVIEW, &count);
        if (d.enumDone || (count > 0 && (UINT)count == d.lastCount) || now - d.seen >= kNewViewDelayMs) {
            d.ready = true;
        } else {
            d.lastCount = (UINT)count;
            Recheck(dialog, kRecheckMs);
            return;
        }
    }

    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    if (FAILED(view->GetGroupBy(&groupKey, &ascending))) return;
    int current = KeyIndex(groupKey);

    if (names.empty() || !enabled) {
        // No pins here, or turned off with the on/off switch: the folder's own grouping.
        if (IsPinKey(groupKey)) {
            RestoreGrouping(d, view.Get());
            RegDeleteValueIn(HKEY_CURRENT_USER, kRegPreviousDialogGroupBy, d.folderPath.c_str());
        }
        d.grouped = false;
        d.lastNames.clear();
        return;
    }
    bool pinsChanged = names != d.lastNames;
    d.lastNames = names;
    if (pinsChanged) {
        d.attempts = 0;
        d.gaveUp = false;
    }
    if (current >= 0 && ascending && KeyMatches(d, view.Get(), folder.Get(), current, names)) {
        d.attempts = 0;
        return;
    }
    // A grouping the user picked in this dialog is kept until the pins change.
    if ((current < 0 && d.grouped && !pinsChanged) || d.gaveUp) return;
    if (d.attempts >= kMaxAttempts) {
        LogLine(L"dialog: grouping does not hold in %s; giving up", d.folderPath.c_str());
        if (IsPinKey(groupKey)) RestoreGrouping(d, view.Get());
        d.gaveUp = true;
        return;
    }
    // At most about one regroup a second, except for a change of the pins (a user action).
    if (!pinsChanged && now - d.lastRegroup < 1000) {
        Recheck(dialog, 1000 - (now - d.lastRegroup));
        return;
    }

    int key = current < 0 ? 0 : 1 - current;
    if (current < 0 && !IsPinKey(groupKey)) SavePreviousGrouping(d.folderPath, groupKey, ascending);
    if (!WriteKey(d, view.Get(), folder.Get(), key, names)) {  // the items are not in the view yet
        Recheck(dialog, 150);
        return;
    }
    d.lastRegroup = now;
    d.attempts++;
    d.grouped = true;
    HRESULT hr = view->SetGroupBy(kPinStateKeys[key], TRUE);
    LogLine(L"dialog: group %s (%zu pinned, attempt %d, 0x%08lX)", d.folderPath.c_str(), names.size(), d.attempts,
            (unsigned long)hr);
}

// ---------------------------------------------------------------- dialog discovery

LRESULT CALLBACK GetMessageHook(int code, WPARAM wp, LPARAM lp) {
    MSG* msg = reinterpret_cast<MSG*>(lp);
    if (code == HC_ACTION && wp == PM_REMOVE) {
        if (msg->message == g_tickMessage && g_tickMessage) {
            HWND dialog = msg->hwnd;
            msg->message = WM_NULL;
            ProcessDialog(dialog);
        } else if (msg->message == WM_TIMER && msg->wParam == kTimerId && t_dialogs && t_dialogs->count(msg->hwnd)) {
            // Our recheck timer: never reaches the dialog's own window procedure.
            HWND dialog = msg->hwnd;
            KillTimer(dialog, kTimerId);
            msg->message = WM_NULL;
            ProcessDialog(dialog);
        } else if (IsCloseCommand(msg->message, msg->wParam)) {
            BeforeLeave(msg->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Sent messages: the buttons, Enter and Esc reach the dialog as a sent WM_COMMAND.
LRESULT CALLBACK CallWndProcHook(int code, WPARAM wp, LPARAM lp) {
    const CWPSTRUCT* msg = reinterpret_cast<const CWPSTRUCT*>(lp);
    if (code == HC_ACTION) {
        if (IsCloseCommand(msg->message, msg->wParam)) {
            BeforeLeave(msg->hwnd);
        } else if (msg->message == WM_NCDESTROY) {
            ForgetDialog(msg->hwnd);
        } else if (msg->message == WM_WINDOWPOSCHANGED && t_buttons && !t_buttons->empty()) {
            // The dialog or its folder view moved or changed size: the button follows.
            auto it = t_buttons->find(GetAncestor(msg->hwnd, GA_ROOT));
            if (it != t_buttons->end() && it->second->Visible()) it->second->Place();
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ---- worker thread: finds file dialogs of this process and tells them about changes
std::set<DWORD>* g_hookedThreads = nullptr;
std::set<HWND>* g_knownDialogs = nullptr;
bool g_settingOn = true;

bool IsFileDialog(HWND hwnd) {
    wchar_t cls[16];
    return IsWindowVisible(hwnd) && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"#32770") == 0 &&
           FindDefView(hwnd);
}

void Visit(HWND dialog) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(dialog, &pid);
    if (pid != GetCurrentProcessId() || !IsFileDialog(dialog)) return;
    if (!g_hookedThreads->count(tid)) {
        if (!g_settingOn) return;
        // A hook on the dialog's thread lets the work run there, where the view lives.
        if (!SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, g_module, tid)) return;
        SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, g_module, tid);
        g_hookedThreads->insert(tid);
    }
    g_knownDialogs->insert(dialog);
    PostMessageW(dialog, g_tickMessage, 0, 0);
}

BOOL CALLBACK ScanWindow(HWND hwnd, LPARAM) {
    Visit(hwnd);
    return TRUE;
}

// A dialog was shown, or a folder view was created or shown in one (a new dialog or a
// navigation). The dialog often shows up before its folder view does.
void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY) {
        g_knownDialogs->erase(hwnd);
        return;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root != hwnd) {
        wchar_t cls[32];
        if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"SHELLDLL_DefView") != 0) return;
    } else if (event != EVENT_OBJECT_SHOW) {
        return;
    }
    if (root) Visit(root);
}

// The pins or the settings changed: every dialog looks again at once.
void TellDialogs() {
    for (auto it = g_knownDialogs->begin(); it != g_knownDialogs->end();) {
        if (!IsWindow(*it)) {
            it = g_knownDialogs->erase(it);
            continue;
        }
        PostMessageW(*it, g_tickMessage, 0, 0);
        ++it;
    }
}

void WatchKey(HKEY key, HANDLE event) {
    if (key) RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event, TRUE);
}

DWORD WINAPI Worker(void*) {
    g_hookedThreads = new std::set<DWORD>();
    g_knownDialogs = new std::set<HWND>();
    g_settingOn = DialogsSettingOn();
    HKEY keys[2] = {};
    RegCreateKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, nullptr, 0, KEY_NOTIFY | KEY_QUERY_VALUE, nullptr, &keys[0], nullptr);
    RegCreateKeyExW(HKEY_CURRENT_USER, kRegPins, 0, nullptr, 0, KEY_NOTIFY | KEY_QUERY_VALUE, nullptr, &keys[1], nullptr);
    HANDLE events[2] = {CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    for (int i = 0; i < 2; i++) WatchKey(keys[i], events[i]);
    // Out-of-context window events of this process arrive through this thread's message queue.
    SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnWinEvent, GetCurrentProcessId(), 0,
                    WINEVENT_OUTOFCONTEXT);
    EnumWindows(ScanWindow, 0);
    DWORD lastScan = GetTickCount();
    for (;;) {
        DWORD since = GetTickCount() - lastScan;
        DWORD r = MsgWaitForMultipleObjects(2, events, FALSE, since >= kScanMs ? 0 : kScanMs - since, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) {
            WatchKey(keys[r - WAIT_OBJECT_0], events[r - WAIT_OBJECT_0]);
            g_settingOn = DialogsSettingOn();
            TellDialogs();
        } else if (r == WAIT_OBJECT_0 + 2) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        // On time even while window events keep coming (a busy program such as a browser or an
        // Electron app sends them all the time).
        if (GetTickCount() - lastScan >= kScanMs) {
            lastScan = GetTickCount();
            g_settingOn = DialogsSettingOn();
            EnumWindows(ScanWindow, 0);
        }
    }
}

// Explorer windows are handled by the agent.
bool ProcessAllowed() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (PathEqualsI(FileNamePart(exe), L"explorer.exe")) return false;
    return GetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", nullptr, 0) == 0;
}

void StartOnce() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (!ProcessAllowed()) return;
    // The hooks and the worker run code from this DLL until the process ends.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&StartOnce), &self);
    g_tickMessage = RegisterWindowMessageW(L"ExplorerPinned.DialogTick");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LogLine(L"dialog support loaded in %s", exe);
    if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
}

// ---------------------------------------------------------------- COM objects

// Icon overlay handler that never shows an overlay: its only purpose is to be loaded.
class PinOverlay : public IShellIconOverlayIdentifier {
public:
    PinOverlay() { InterlockedIncrement(&g_objects); }
    virtual ~PinOverlay() { InterlockedDecrement(&g_objects); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IShellIconOverlayIdentifier) {
            *ppv = static_cast<IShellIconOverlayIdentifier*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP IsMemberOf(PCWSTR, DWORD) override { return S_FALSE; }
    // No overlay image: Windows then drops this handler without using one of its overlay slots.
    STDMETHODIMP GetOverlayInfo(PWSTR, int, int*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP GetPriority(int* priority) override {
        *priority = 100;
        return S_OK;
    }

private:
    LONG ref_ = 1;
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        StartOnce();
        PinOverlay* overlay = new PinOverlay();
        HRESULT hr = overlay->QueryInterface(riid, ppv);
        overlay->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock)
            InterlockedIncrement(&g_objects);
        else
            InterlockedDecrement(&g_objects);
        return S_OK;
    }
};

Factory g_factory;

}  // namespace
}  // namespace ep

using namespace ep;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != kOverlayClsid) {
        *ppv = nullptr;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return g_factory.QueryInterface(riid, ppv);
}

// Stays loaded once dialog support has started (see StartOnce).
STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

// regsvr32 support; the program registers the DLLs itself (see dialogs.cpp).
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    return RegisterDialogExtension(0, path) ? S_OK : E_ACCESSDENIED;
}

STDAPI DllUnregisterServer() {
    UnregisterDialogExtension(0);
    return S_OK;
}
