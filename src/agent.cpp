#include "agent.h"

#include <exdisp.h>
#include <exdispid.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propvarutil.h>

#include <algorithm>
#include <memory>
#include <set>

#include "comutil.h"
#include "displaycheck.h"
#include "pinstore.h"
#include "resource.h"
#include "setup.h"
#include "util.h"
#include "version.h"

namespace ep {
namespace {

// DShellFolderViewEvents
constexpr DISPID kDispSelectionChanged = 200;
constexpr DISPID kDispEnumDone = 201;  // DISPID_FILELISTENUMDONE (also after a refresh or a regroup)
constexpr DISPID kDispFocusChanged = 208;

constexpr UINT_PTR kTimerWork = 1;
constexpr UINT_PTR kTimerPoll = 2;
constexpr UINT kPollIntervalMs = 2000;

// Explorer loads a view after a navigation and reloads it after every SetGroupBy; in a large
// or slow folder (e.g. on the network, or while signing in) that takes seconds and ends with
// DocumentComplete and EnumDone. A SetGroupBy made while a load is still running is undone
// when that load finishes: the pinned values are dropped and every item is "Unspecified".
// So the agent changes the grouping only when the view has finished loading and has had no
// load for kQuietMs (kIdleMs after Explorer dropped the values once).
constexpr DWORD kQuietMs = 400;
constexpr DWORD kIdleMs = 3000;
constexpr DWORD kMaxLoadMs = 15000;  // a load whose end was not reported, or a view that keeps loading
// Groupings applied to one view that did not hold before the agent stops trying and puts
// the original grouping back (instead of reloading the view endlessly).
constexpr int kMaxAttempts = 4;

constexpr UINT kCmdReapply = 1;
constexpr UINT kCmdCleanup = 2;
constexpr UINT kCmdStartup = 3;
constexpr UINT kCmdWebsite = 4;
constexpr UINT kCmdExit = 5;
constexpr UINT kCmdOpenBase = 1000;   // + index into the pin list
constexpr UINT kCmdUnpinBase = 3000;  // + index into the pin list
constexpr size_t kMaxMenuPins = 100;

struct LessI {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
    }
};
using NameSet = std::set<std::wstring, LessI>;

// IFolderView2::SetViewProperty/GetViewProperty are marked deprecated in the Windows SDK
// but are implemented by Explorer on Windows 10 and 11; they are the only way to give
// items of an ordinary folder a value to group by without touching the files.
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

// Also the keys of version 1.0.x, which Explorer may still have saved for a folder.
bool IsPinKey(const PROPERTYKEY& key) {
    return KeyIndex(key) >= 0 || IsEqualPropertyKey(key, kLegacyPinStateKeys[0]) ||
           IsEqualPropertyKey(key, kLegacyPinStateKeys[1]);
}

bool IsDisconnected(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == CO_E_OBJNOTCONNECTED;
}

// Grouping that a folder had before the agent took it over, restored when the
// folder no longer contains pins.
void SavePreviousGroupBy(const std::wstring& folder, PROPERTYKEY key, BOOL ascending) {
    std::wstring existing;
    if (RegReadString(HKEY_CURRENT_USER, kRegPreviousGroupBy, folder.c_str(), &existing)) return;
    if (IsPinKey(key)) key = PROPERTYKEY{GUID_NULL, 0};
    wchar_t guid[64];
    StringFromGUID2(key.fmtid, guid, ARRAYSIZE(guid));
    RegWriteString(HKEY_CURRENT_USER, kRegPreviousGroupBy, folder.c_str(),
                   std::wstring(guid) + L"," + std::to_wstring(key.pid) + L"," + (ascending ? L"1" : L"0"));
}

bool ReadPreviousGroupBy(const std::wstring& folder, PROPERTYKEY* key, BOOL* ascending) {
    std::wstring value;
    *key = PROPERTYKEY{GUID_NULL, 0};
    *ascending = TRUE;
    if (!RegReadString(HKEY_CURRENT_USER, kRegPreviousGroupBy, folder.c_str(), &value)) return false;
    size_t c1 = value.find(L',');
    size_t c2 = value.find(L',', c1 + 1);
    if (c1 == std::wstring::npos || c2 == std::wstring::npos) return false;
    GUID g;
    if (FAILED(CLSIDFromString(value.substr(0, c1).c_str(), &g))) return false;
    key->fmtid = g;
    key->pid = (DWORD)wcstoul(value.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
    *ascending = value.substr(c2 + 1) != L"0";
    if (IsPinKey(*key)) *key = PROPERTYKEY{GUID_NULL, 0};  // never "restore" to our own grouping
    return true;
}

// Explorer saves a folder's view state when the view goes away, keyed by the ID list the
// window used for the folder (a folder can be reached through different ID lists). Each view
// grouped by pins is recorded with its ID list, so that a pinned grouping Explorer saved can
// be undone on exit and uninstall. Value name: "<folder>|<n>", data: the ID list.
struct GroupedEntry {
    std::wstring valueName;
    std::wstring folder;
    std::vector<BYTE> idList;
};

std::vector<GroupedEntry> GroupedEntries() {
    std::vector<GroupedEntry> entries;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegGroupedFolders, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return entries;
    std::vector<wchar_t> name(32768);
    std::vector<BYTE> data(65536);
    for (DWORD i = 0;; i++) {
        DWORD nameLen = (DWORD)name.size(), dataLen = (DWORD)data.size(), type = 0;
        if (RegEnumValueW(key, i, name.data(), &nameLen, nullptr, &type, data.data(), &dataLen) != ERROR_SUCCESS) break;
        GroupedEntry e;
        e.valueName.assign(name.data(), nameLen);
        size_t bar = e.valueName.rfind(L'|');
        e.folder = bar == std::wstring::npos ? e.valueName : e.valueName.substr(0, bar);
        if (type == REG_BINARY) e.idList.assign(data.begin(), data.begin() + dataLen);
        entries.push_back(std::move(e));
    }
    RegCloseKey(key);
    return entries;
}

void MarkGrouped(const std::wstring& folder, PCIDLIST_ABSOLUTE idList) {
    UINT size = ILGetSize(idList);
    auto bytes = reinterpret_cast<const BYTE*>(idList);
    for (const auto& e : GroupedEntries())
        if (PathEqualsI(e.folder, folder) && e.idList.size() == size && memcmp(e.idList.data(), bytes, size) == 0) return;
    for (int n = 0;; n++) {
        std::wstring name = folder + L"|" + std::to_wstring(n);
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegGroupedFolders, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                            nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        bool taken = RegQueryValueExW(key, name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        if (!taken) RegSetValueExW(key, name.c_str(), 0, REG_BINARY, bytes, size);
        RegCloseKey(key);
        if (!taken) return;
    }
}

void ClearGrouped(const std::wstring& folder) {
    for (const auto& e : GroupedEntries())
        if (PathEqualsI(e.folder, folder)) RegDeleteValueIn(HKEY_CURRENT_USER, kRegGroupedFolders, e.valueName.c_str());
}


struct TrackedWindow {
    ComPtr<IWebBrowser2> browser;
    ComPtr<IUnknown> identity;
    EventConnection browserEvents;
    EventConnection viewEvents;

    // Current view ("session"); a navigation creates a new view object.
    ComPtr<IFolderView2> view;
    ComPtr<IUnknown> viewIdentity;
    ComPtr<IShellFolder> folder;
    std::wstring folderPath;
    std::vector<BYTE> folderIdList;  // the ID list this window uses for the folder
    NameSet applied[2];       // names given the pinned value on each key in this view
    NameSet unwritable[2];    // pinned names the view refused a value for (e.g. hidden items)
    bool initialized = false;  // grouping was enforced once in this view

    bool pending = false;
    bool enforce = false;
    DWORD due = 0;
    DWORD lastRegroup = 0;
    bool leaving = false;  // the original grouping was put back because the view is going away
    DWORD leaveTick = 0;
    bool seen = false;

    bool loading = false;    // a navigation or our SetGroupBy is being loaded
    DWORD loadStart = 0;
    DWORD lastLoad = 0;      // last time a load started or finished
    DWORD waitSince = 0;     // first time processing was put off for a load
    int attempts = 0;        // groupings applied in this view since the grouping last held
    int churn = 0;           // regroups in a row that came soon after the previous one
    int writeFailures = 0;   // tries in a row in which the view took no pinned value
    bool dropped = false;    // Explorer dropped the values of the grouping applied last
    bool gaveUp = false;     // the grouping did not hold; left alone until the pins change
    bool displayLogged = false;  // what Explorer shows was written to the log
};

class Agent {
public:
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

    void Schedule(TrackedWindow* w, DWORD delayMs, bool enforce);
    void ArmWorkTimer();
    void DoWork();

    bool EnsureShellWindows();
    void DropShellWindows();
    void RescanWindows();
    TrackedWindow* FindTracked(IUnknown* identity);

    void ProcessWindow(TrackedWindow& w);
    bool AcquireView(TrackedWindow& w, bool* newSession);
    bool Settling(TrackedWindow& w, DWORD* waitMs);
    PITEMID_CHILD ParseChild(TrackedWindow& w, const std::wstring& name);
    size_t WriteKey(TrackedWindow& w, int keyIndex, const NameSet& names, HRESULT* failure);
    bool KeyMatches(TrackedWindow& w, int keyIndex, const NameSet& names);
    void RestoreGrouping(TrackedWindow& w, bool forget);
    void LeaveView(TrackedWindow& w);
    void RestoreAll();

    void ArmPinsWatch();
    void OnPinsChanged();

    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void OnCommand(UINT id);

    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool busy_ = false;
    bool dropPending_ = false;
    bool restoreOnExit_ = false;
    bool restoreClosedOnExit_ = false;

    ComPtr<IShellWindows> shellWindows_;
    EventConnection shellWindowsEvents_;
    std::vector<std::unique_ptr<TrackedWindow>> windows_;
    bool rescanPending_ = true;

    std::vector<std::wstring> pins_;
    std::vector<std::wstring> menuPins_;
    HKEY pinsKey_ = nullptr;
    HANDLE pinsEvent_ = nullptr;
};

Agent* g_agent = nullptr;

// Version, Windows build and the state of the property schema, for diagnosing problems.
void LogEnvironment(size_t pinCount) {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product, display, build;
    RegReadString(HKEY_LOCAL_MACHINE, nt, L"ProductName", &product);
    RegReadString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion", &display);
    RegReadString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", &build);
    DWORD ubr = 0, size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, nt, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    LogLine(L"agent " EP_VERSION_STRING_W L" started on %s %s (build %s.%lu), %zu pin(s)", product.c_str(), display.c_str(),
            build.c_str(), ubr, pinCount);
    for (const auto& key : kPinStateKeys) {
        IPropertyDescription* desc = nullptr;
        HRESULT hr = PSGetPropertyDescription(key, IID_PPV_ARGS(&desc));
        if (FAILED(hr)) {
            LogLine(L"  property %lu: not available (0x%08lX)", key.pid, (unsigned long)hr);
            continue;
        }
        PWSTR text = nullptr;
        PROPVARIANT pinned;
        InitPropVariantFromUInt32(kPinnedValue, &pinned);
        desc->FormatForDisplay(pinned, PDFF_DEFAULT, &text);
        LogLine(L"  property %lu: \"%s\"", key.pid, text ? text : L"?");
        CoTaskMemFree(text);
        desc->Release();
    }
    for (const auto& r : SchemaRegistrations())
        LogLine(L"  schema %s%s%s", r.path.c_str(), r.legacy ? L" (old)" : L"", r.exists ? L"" : L" (missing)");
}

LRESULT CALLBACK Agent::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_agent && g_agent->hwnd_ == hwnd) return g_agent->OnMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Agent::Run() {
    g_agent = this;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kAgentWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window: it receives the TaskbarCreated broadcast and
    // session-end messages, which message-only windows do not.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAgentWindowClass, L"Explorer Pinned", WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, inst, nullptr);
    if (!hwnd_) return 1;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, kMsgReapply, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, kMsgExit, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);

    RegCreateKeyExW(HKEY_CURRENT_USER, kRegPins, 0, nullptr, 0, KEY_NOTIFY | KEY_QUERY_VALUE, nullptr, &pinsKey_,
                    nullptr);
    pinsEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pins_ = PinStore::LoadAll();
    ArmPinsWatch();

    AddTrayIcon();
    SetTimer(hwnd_, kTimerPoll, kPollIntervalMs, nullptr);
    rescanPending_ = true;
    ArmWorkTimer();
    LogEnvironment(pins_.size());

    MSG msg;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjectsEx(pinsEvent_ ? 1 : 0, &pinsEvent_, INFINITE, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (pinsEvent_ && r == WAIT_OBJECT_0) {
            OnPinsChanged();
            continue;
        }
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }

    if (restoreOnExit_) {
        RestoreAll();
        // Folders whose saved view still has the pinned grouping (rare: Explorer closed while
        // the agent was not running) are fixed through Explorer windows; that fails from this
        // process while it shuts down, so a helper process does it.
        if (restoreClosedOnExit_ && !GroupedEntries().empty()) {
            std::wstring cmd = L"\"" + ExePath() + L"\" restore-groupings";
            STARTUPINFOW si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    windows_.clear();
    shellWindowsEvents_.Reset();
    shellWindows_.Reset();
    RemoveTrayIcon();
    if (pinsKey_) RegCloseKey(pinsKey_);
    if (pinsEvent_) CloseHandle(pinsEvent_);
    DestroyWindow(hwnd_);
    g_agent = nullptr;
    LogLine(L"agent stopped");
    return (int)msg.wParam;
}

LRESULT Agent::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerPoll) {
                rescanPending_ = true;
                for (auto& w : windows_) Schedule(w.get(), 0, false);
            } else if (wp == kTimerWork) {
                KillTimer(hwnd_, kTimerWork);
            }
            DoWork();
            return 0;
        case kMsgReapply:
            pins_ = PinStore::LoadAll();
            rescanPending_ = true;
            for (auto& w : windows_) Schedule(w.get(), 0, true);
            ArmWorkTimer();
            return 0;
        case kMsgExit:
            // wParam: 0 = just exit, 1 = restore open windows (the caller restores the rest),
            // 2 = restore open windows and closed folders.
            restoreOnExit_ = wp != 0;
            restoreClosedOnExit_ = wp == 2;
            PostQuitMessage(0);
            return 0;
        case kMsgTray:
            if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == NIN_SELECT ||
                LOWORD(lp) == NIN_KEYSELECT)
                ShowTrayMenu();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == taskbarCreated_ && taskbarCreated_) {
                trayAdded_ = false;
                AddTrayIcon();
                // Explorer restarted: every proxy we hold is dead.
                dropPending_ = true;
                rescanPending_ = true;
                ArmWorkTimer();
                return 0;
            }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Agent::Schedule(TrackedWindow* w, DWORD delayMs, bool enforce) {
    DWORD due = GetTickCount() + delayMs;
    if (!w->pending || (int)(due - w->due) < 0) w->due = due;
    w->pending = true;
    w->enforce = w->enforce || enforce;
}

void Agent::ArmWorkTimer() {
    DWORD now = GetTickCount();
    bool any = rescanPending_;
    DWORD next = now;
    for (auto& w : windows_) {
        if (!w->pending) continue;
        if (!any || (int)(w->due - next) < 0) next = w->due;
        any = true;
    }
    if (!any) return;
    int delay = std::max(0, (int)(next - now));
    if (rescanPending_) delay = 0;
    SetTimer(hwnd_, kTimerWork, (UINT)std::max(delay, 10), nullptr);
}

void Agent::DoWork() {
    // Cross-process calls below run a modal loop; don't re-enter.
    if (busy_) return;
    busy_ = true;
    if (dropPending_) {
        dropPending_ = false;
        DropShellWindows();
    }
    if (rescanPending_) {
        rescanPending_ = false;
        RescanWindows();
    }
    DWORD now = GetTickCount();
    for (size_t i = 0; i < windows_.size(); i++) {
        TrackedWindow& w = *windows_[i];
        if (!w.pending || (int)(w.due - now) > 0) continue;
        w.pending = false;
        ProcessWindow(w);
    }
    busy_ = false;
    ArmWorkTimer();
}

bool Agent::EnsureShellWindows() {
    if (shellWindows_) return true;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(shellWindows_.Put()))))
        return false;
    shellWindowsEvents_.Connect(shellWindows_.Get(), DIID_DShellWindowsEvents, [this](DISPID) {
        rescanPending_ = true;
        ArmWorkTimer();
    });
    return true;
}

void Agent::DropShellWindows() {
    windows_.clear();
    shellWindowsEvents_.Reset();
    shellWindows_.Reset();
}

TrackedWindow* Agent::FindTracked(IUnknown* identity) {
    for (auto& w : windows_)
        if (w->identity.Get() == identity) return w.get();
    return nullptr;
}

void Agent::RescanWindows() {
    if (!EnsureShellWindows()) return;
    long count = 0;
    HRESULT hr = shellWindows_->get_Count(&count);
    if (FAILED(hr)) {
        if (IsDisconnected(hr)) DropShellWindows();
        return;
    }
    for (auto& w : windows_) w->seen = false;
    for (long i = 0; i < count; i++) {
        VARIANT index;
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> disp;
        if (FAILED(shellWindows_->Item(index, disp.Put())) || !disp) continue;
        ComPtr<IWebBrowser2> browser = disp.As<IWebBrowser2>();
        if (!browser) continue;
        ComPtr<IUnknown> id = Identity(browser.Get());
        if (TrackedWindow* existing = FindTracked(id.Get())) {
            existing->seen = true;
            continue;
        }
        auto w = std::make_unique<TrackedWindow>();
        w->browser = browser;
        w->identity = id;
        w->seen = true;
        TrackedWindow* raw = w.get();
        w->browserEvents.Connect(browser.Get(), DIID_DWebBrowserEvents2, [this, raw](DISPID id) {
            if (id == DISPID_ONQUIT) {
                // Explorer saves the view state when the window closes; put the original
                // grouping back first (synchronously, while Explorer waits for this event).
                // (Explorer does not raise BeforeNavigate2 for folder navigation.)
                LeaveView(*raw);
            }
            if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE) {
                // A navigation starts loading the new view; the next DocumentComplete (or
                // EnumDone) reports that the load is done.
                raw->loading = id == DISPID_NAVIGATECOMPLETE2;
                raw->lastLoad = GetTickCount();
                if (raw->loading) raw->loadStart = raw->lastLoad;
                Schedule(raw, kQuietMs, false);
                ArmWorkTimer();
            } else if (id == DISPID_ONQUIT) {
                rescanPending_ = true;
                ArmWorkTimer();
            }
        });
        Schedule(raw, 0, false);
        windows_.push_back(std::move(w));
    }
    windows_.erase(std::remove_if(windows_.begin(), windows_.end(), [](const auto& w) { return !w->seen; }),
                   windows_.end());
}

bool Agent::AcquireView(TrackedWindow& w, bool* newSession) {
    *newSession = false;
    ComPtr<IServiceProvider> sp = w.browser.As<IServiceProvider>();
    ComPtr<IShellBrowser> sb;
    ComPtr<IShellView> sv;
    HRESULT hr = sp ? sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(sb.Put())) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = sb->QueryActiveShellView(sv.Put());
    ComPtr<IFolderView2> view = sv ? sv.As<IFolderView2>() : ComPtr<IFolderView2>();
    if (!view) {
        if (IsDisconnected(hr)) rescanPending_ = true;
        w.view.Reset();
        w.viewIdentity.Reset();
        w.folder.Reset();
        w.folderPath.clear();
        w.viewEvents.Reset();
        return false;
    }
    ComPtr<IUnknown> id = Identity(view.Get());
    if (id.Get() == w.viewIdentity.Get()) return true;

    // New view: new navigation (values set on the old view are gone).
    *newSession = true;
    w.view = view;
    w.viewIdentity = id;
    w.folder.Reset();
    w.folderPath.clear();
    w.folderIdList.clear();
    w.applied[0].clear();
    w.applied[1].clear();
    w.unwritable[0].clear();
    w.unwritable[1].clear();
    w.initialized = false;
    w.leaving = false;
    w.waitSince = 0;
    w.attempts = 0;
    w.churn = 0;
    w.writeFailures = 0;
    w.dropped = false;
    w.gaveUp = false;
    w.displayLogged = false;
    // A view found without seeing its navigation (e.g. when the agent starts) may still be
    // loading; give it a moment.
    if (!w.loading) w.lastLoad = GetTickCount();
    view->GetFolder(IID_PPV_ARGS(w.folder.Put()));
    if (ComPtr<IPersistFolder2> pf = w.folder.As<IPersistFolder2>()) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(pf->GetCurFolder(&pidl))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path))) {
                w.folderPath = NormalizePath(path);
                CoTaskMemFree(path);
            }
            auto bytes = reinterpret_cast<const BYTE*>(pidl);
            w.folderIdList.assign(bytes, bytes + ILGetSize(pidl));
            CoTaskMemFree(pidl);
        }
    }
    ComPtr<IDispatch> doc;
    TrackedWindow* raw = &w;
    if (SUCCEEDED(w.browser->get_Document(doc.Put())) && doc) {
        w.viewEvents.Connect(doc.Get(), DIID_DShellFolderViewEvents, [this, raw](DISPID id) {
            if (id == kDispSelectionChanged || id == kDispFocusChanged) return;
            if (id == kDispEnumDone) {
                raw->loading = false;
                raw->lastLoad = GetTickCount();
            }
            Schedule(raw, kQuietMs, false);
            ArmWorkTimer();
        });
    }
    return true;
}

PITEMID_CHILD Agent::ParseChild(TrackedWindow& w, const std::wstring& name) {
    PIDLIST_RELATIVE pidl = nullptr;
    ULONG eaten = 0;
    std::wstring buf = name;
    if (FAILED(w.folder->ParseDisplayName(nullptr, nullptr, buf.data(), &eaten, &pidl, nullptr))) return nullptr;
    if (!ILIsChild(pidl)) {
        CoTaskMemFree(pidl);
        return nullptr;
    }
    return static_cast<PITEMID_CHILD>(pidl);
}

// Gives `names` the pinned value on the key and clears names pinned earlier in this view.
// Returns how many pinned items got the value; `failure` receives the last error.
size_t Agent::WriteKey(TrackedWindow& w, int k, const NameSet& names, HRESULT* failure) {
    const PROPERTYKEY& key = kPinStateKeys[k];
    *failure = S_OK;
    PROPVARIANT empty;
    PropVariantInit(&empty);
    for (const auto& name : w.applied[k]) {
        if (names.count(name)) continue;
        if (PITEMID_CHILD child = ParseChild(w, name)) {
            SetViewValue(w.view.Get(), child, key, empty);
            CoTaskMemFree(child);
        }
    }
    w.applied[k].clear();
    w.unwritable[k].clear();
    PROPVARIANT pinned;
    InitPropVariantFromUInt32(kPinnedValue, &pinned);
    for (const auto& name : names) {
        if (PITEMID_CHILD child = ParseChild(w, name)) {
            HRESULT hr = SetViewValue(w.view.Get(), child, key, pinned);
            if (SUCCEEDED(hr)) {
                w.applied[k].insert(name);
            } else {
                w.unwritable[k].insert(name);
                *failure = hr;
            }
            CoTaskMemFree(child);
        }
    }
    return w.applied[k].size();
}

// True when the active key already shows exactly the pinned items. Values disappear
// on refresh and when an item changes, so they are read back rather than assumed.
bool Agent::KeyMatches(TrackedWindow& w, int k, const NameSet& names) {
    for (const auto& name : w.applied[k])
        if (!names.count(name)) return false;
    for (const auto& name : names) {
        if (w.unwritable[k].count(name)) continue;  // not in the view (e.g. hidden); nothing to show
        PITEMID_CHILD child = ParseChild(w, name);
        if (!child) continue;  // not present in this folder (yet)
        PROPVARIANT value;
        PropVariantInit(&value);
        HRESULT hr = GetViewValue(w.view.Get(), child, kPinStateKeys[k], &value);
        // TYPE_E_ELEMENTNOTFOUND: the view has the item but no value for it (Explorer dropped
        // it, or the item was pinned since). Other failures mean the item is not in the view.
        bool ok = (FAILED(hr) && hr != TYPE_E_ELEMENTNOTFOUND) || (value.vt == VT_UI4 && value.ulVal == kPinnedValue);
        PropVariantClear(&value);
        CoTaskMemFree(child);
        if (!ok) return false;
    }
    return true;
}

// True while the view is loading or a load ended only recently; *waitMs tells when to look
// again. A view that never calms down is processed anyway after a while.
bool Agent::Settling(TrackedWindow& w, DWORD* waitMs) {
    DWORD now = GetTickCount();
    DWORD quiet = w.dropped ? kIdleMs : kQuietMs;
    DWORD since = now - w.lastLoad;
    if ((!w.loading || now - w.loadStart >= kMaxLoadMs) && since >= quiet) {
        w.loading = false;
        w.waitSince = 0;
        return false;
    }
    if (!w.waitSince) w.waitSince = now;
    if (now - w.waitSince >= kMaxLoadMs) {
        w.loading = false;
        w.waitSince = 0;
        return false;
    }
    *waitMs = w.loading ? 250 : quiet - since;
    return true;
}

void Agent::ProcessWindow(TrackedWindow& w) {
    bool newSession = false;
    if (!AcquireView(w, &newSession) || !w.folder || w.folderPath.empty()) return;
    if (w.leaving) {
        // Still the same view: the navigation is in progress, or it was cancelled.
        if (GetTickCount() - w.leaveTick < 2000) {
            Schedule(&w, 500, false);
            return;
        }
        w.view->SetRedraw(TRUE);
        w.leaving = false;
        w.enforce = true;
    }
    DWORD wait = 0;
    if (Settling(w, &wait)) {
        Schedule(&w, wait, false);
        return;
    }

    // Only pins whose item exists count; a stale pin must not keep the folder grouped.
    NameSet names;
    for (auto& n : PinStore::NamesInFolder(pins_, w.folderPath)) {
        if (PITEMID_CHILD child = ParseChild(w, n)) {
            names.insert(n);
            CoTaskMemFree(child);
        }
    }

    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    HRESULT hr = w.view->GetGroupBy(&groupKey, &ascending);
    if (FAILED(hr)) {
        if (IsDisconnected(hr)) rescanPending_ = true;
        return;
    }
    int current = KeyIndex(groupKey);

    if (names.empty()) {
        if (IsPinKey(groupKey)) RestoreGrouping(w, /*forget=*/true);
        // Take over again as soon as a pinned item shows up in this view.
        w.initialized = false;
        w.enforce = false;
        w.attempts = 0;
        w.dropped = false;
        w.gaveUp = false;
        return;
    }

    if (w.enforce) {
        // The pins changed (or "reapply"): start over, also in a view the agent gave up on.
        w.displayLogged = false;
        w.attempts = 0;
        w.churn = 0;
        w.dropped = false;
        w.gaveUp = false;
    }
    if (current >= 0 && ascending && KeyMatches(w, current, names)) {
        w.attempts = 0;
        w.dropped = false;
        w.initialized = true;
        w.enforce = false;
        if (!w.displayLogged) {
            // Record what Explorer actually shows (the values can be in place while Explorer
            // still lists every item as "Unspecified", e.g. when the schema is not usable).
            w.displayLogged = true;
            SHANDLE_PTR hwnd = 0;
            if (SUCCEEDED(w.browser->get_HWND(&hwnd)) && hwnd) LogDisplayedGroups(reinterpret_cast<HWND>(hwnd), w.folderPath);
        }
        return;
    }
    // Respect a grouping the user picked in this view; take over again on a new view or a
    // pin change.
    if ((current < 0 && w.initialized && !w.enforce) || w.gaveUp) return;

    if (w.attempts > 0 && !w.dropped) {
        // The grouping applied last did not hold: Explorer reloaded the view and dropped the
        // values (or put the saved grouping back). Try again once the view has been idle.
        w.dropped = true;
        LogLine(L"grouping did not hold in %s (attempt %d); waiting for the view to be idle", w.folderPath.c_str(),
                w.attempts);
        Schedule(&w, kIdleMs, false);
        return;
    }
    if (w.attempts >= kMaxAttempts) {
        // Stop instead of reloading the view forever, and don't leave every item in the
        // "Unspecified" group.
        LogLine(L"grouping does not hold in %s after %d attempts; giving up", w.folderPath.c_str(), w.attempts);
        if (IsPinKey(groupKey)) RestoreGrouping(w, /*forget=*/false);
        w.gaveUp = true;
        w.enforce = false;
        return;
    }
    // A pinned item that keeps changing (e.g. a file that is being written) loses its value
    // each time. Regroup at most about once a second, and less and less often (down to twice
    // a minute) while that goes on, since every regroup makes Explorer reload the view.
    DWORD now = GetTickCount();
    DWORD gap = w.churn < 3 ? 1000 : std::min<DWORD>(30000, 1000u << std::min(w.churn - 2, 5));
    if (now - w.lastRegroup < gap) {
        Schedule(&w, gap - (now - w.lastRegroup), false);
        return;
    }
    w.churn = w.lastRegroup && now - w.lastRegroup < gap + 10000 ? w.churn + 1 : 0;

    // Explorer only re-groups items when the group-by key changes: write the values to the
    // key that is not active and switch to it.
    int key = current < 0 ? 0 : 1 - current;
    if (current < 0) SavePreviousGroupBy(w.folderPath, groupKey, ascending);
    HRESULT failure = S_OK;
    size_t written = WriteKey(w, key, names, &failure);
    if (!written) {
        // The view does not have the items yet (Explorer is still filling it) or does not
        // show them (hidden items). Grouping now would only show "Unspecified": try again
        // every second for a while, then every ten seconds, without counting it as a
        // failed grouping.
        if (++w.writeFailures == 1 || w.writeFailures % 10 == 0)
            LogLine(L"cannot set the pinned value in %s yet (0x%08lX, try %d)", w.folderPath.c_str(),
                    (unsigned long)failure, w.writeFailures);
        Schedule(&w, w.writeFailures < 20 ? 1000 : 10000, false);
        return;
    }
    w.writeFailures = 0;
    w.lastRegroup = now;
    w.attempts++;
    w.dropped = false;
    w.initialized = true;
    w.enforce = false;
    if (current < 0 && !w.folderIdList.empty())
        MarkGrouped(w.folderPath, reinterpret_cast<PCIDLIST_ABSOLUTE>(w.folderIdList.data()));
    // Explorer reloads the view for the new grouping; check the result once that is done.
    w.loading = true;
    w.loadStart = now;
    w.lastLoad = now;
    hr = w.view->SetGroupBy(kPinStateKeys[key], TRUE);
    LogLine(L"%s %s (%zu of %zu pinned, attempt %d, 0x%08lX)", current < 0 ? L"group" : L"regroup", w.folderPath.c_str(),
            written, names.size(), w.attempts, (unsigned long)hr);
    Schedule(&w, kQuietMs, false);
}

// Puts back the grouping the folder had before. `forget` drops the saved grouping (the
// folder no longer has pins).
void Agent::RestoreGrouping(TrackedWindow& w, bool forget) {
    PROPERTYKEY key;
    BOOL ascending;
    ReadPreviousGroupBy(w.folderPath, &key, &ascending);
    if (forget) RegDeleteValueIn(HKEY_CURRENT_USER, kRegPreviousGroupBy, w.folderPath.c_str());
    w.loading = true;  // Explorer reloads the view for this grouping too
    w.loadStart = w.lastLoad = GetTickCount();
    w.view->SetGroupBy(key, ascending);
    ClearGrouped(w.folderPath);
    // The values stay in the view's cache; `applied` keeps tracking them so they are
    // cleared when the folder is grouped again.
    LogLine(L"restore grouping %s", w.folderPath.c_str());
}

// Called while Explorer is about to navigate away from the view or close the window: the
// view state it saves then must not contain the pinned grouping.
void Agent::LeaveView(TrackedWindow& w) {
    if (!w.view || w.folderPath.empty() || w.leaving) return;
    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    if (FAILED(w.view->GetGroupBy(&groupKey, &ascending)) || !IsPinKey(groupKey)) return;
    w.view->SetRedraw(FALSE);  // the view is about to go away; don't show the regrouping
    RestoreGrouping(w, /*forget=*/false);
    w.leaving = true;
    w.leaveTick = GetTickCount();
}

void Agent::RestoreAll() {
    for (auto& w : windows_) {
        if (!w->view || w->folderPath.empty()) continue;
        PROPERTYKEY groupKey = {};
        BOOL ascending = TRUE;
        if (SUCCEEDED(w->view->GetGroupBy(&groupKey, &ascending)) && IsPinKey(groupKey))
            RestoreGrouping(*w, /*forget=*/false);
    }
}

void Agent::ArmPinsWatch() {
    if (pinsKey_ && pinsEvent_)
        RegNotifyChangeKeyValue(pinsKey_, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, pinsEvent_,
                                TRUE);
}

void Agent::OnPinsChanged() {
    ArmPinsWatch();
    std::vector<std::wstring> old = std::move(pins_);
    pins_ = PinStore::LoadAll();
    // Several "pin" processes may run at once (multiple selection); whichever wrote the
    // menu conditions last may have missed a pin, so rewrite them from the final list.
    UpdateContextMenu(pins_);
    for (auto& w : windows_) {
        if (w->folderPath.empty()) continue;
        auto before = PinStore::NamesInFolder(old, w->folderPath);
        auto after = PinStore::NamesInFolder(pins_, w->folderPath);
        NameSet a(before.begin(), before.end()), b(after.begin(), after.end());
        if (a != b) Schedule(w.get(), 0, true);
    }
    ArmWorkTimer();
}

void Agent::AddTrayIcon() {
    if (trayAdded_) return;
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &nid.hIcon);
    wcsncpy_s(nid.szTip, LoadStr(IDS_APP_NAME).c_str(), _TRUNCATE);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (trayAdded_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

void Agent::RemoveTrayIcon() {
    if (!trayAdded_) return;
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded_ = false;
}

void Agent::ShowTrayMenu() {
    menuPins_ = PinStore::LoadAll();
    std::sort(menuPins_.begin(), menuPins_.end(), LessI());
    HMENU menu = CreatePopupMenu();
    HMENU pinsMenu = CreatePopupMenu();
    if (menuPins_.empty()) {
        AppendMenuW(pinsMenu, MF_STRING | MF_GRAYED, 0, LoadStr(IDS_TRAY_NONE).c_str());
    }
    for (size_t i = 0; i < menuPins_.size() && i < kMaxMenuPins; i++) {
        HMENU item = CreatePopupMenu();
        AppendMenuW(item, MF_STRING, kCmdOpenBase + i, LoadStr(IDS_TRAY_OPEN_ITEM).c_str());
        AppendMenuW(item, MF_STRING, kCmdUnpinBase + i, LoadStr(IDS_TRAY_UNPIN_ITEM).c_str());
        std::wstring label = FileNamePart(menuPins_[i]) + L"\t" + ParentPath(menuPins_[i]);
        // '&' would turn into a mnemonic.
        for (size_t p = 0; (p = label.find(L'&', p)) != std::wstring::npos; p += 2) label.insert(p, 1, L'&');
        AppendMenuW(pinsMenu, MF_POPUP | (PathExists(menuPins_[i]) ? 0 : MF_GRAYED), (UINT_PTR)item, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)pinsMenu, LoadStr(IDS_TRAY_PINNED_ITEMS).c_str());
    AppendMenuW(menu, MF_STRING, kCmdReapply, LoadStr(IDS_TRAY_REAPPLY).c_str());
    AppendMenuW(menu, MF_STRING, kCmdCleanup, LoadStr(IDS_TRAY_CLEANUP).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kCmdStartup, LoadStr(IDS_TRAY_STARTUP).c_str());
    AppendMenuW(menu, MF_STRING, kCmdWebsite, LoadStr(IDS_TRAY_WEBSITE).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, LoadStr(IDS_TRAY_EXIT).c_str());

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) OnCommand(cmd);
}

void Agent::OnCommand(UINT id) {
    if (id >= kCmdUnpinBase && id < kCmdUnpinBase + kMaxMenuPins) {
        size_t i = id - kCmdUnpinBase;
        if (i < menuPins_.size()) {
            PinStore::Remove(menuPins_[i]);
            UpdateContextMenu(PinStore::LoadAll());
        }
        return;
    }
    if (id >= kCmdOpenBase && id < kCmdOpenBase + kMaxMenuPins) {
        size_t i = id - kCmdOpenBase;
        if (i < menuPins_.size()) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(menuPins_[i].c_str(), nullptr, &pidl, 0, nullptr))) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                CoTaskMemFree(pidl);
            }
        }
        return;
    }
    switch (id) {
        case kCmdReapply:
            PostMessageW(hwnd_, kMsgReapply, 0, 0);
            break;
        case kCmdCleanup: {
            int removed = PinStore::RemoveMissing();
            UpdateContextMenu(PinStore::LoadAll());
            MessageBoxW(hwnd_, FormatStr(IDS_MSG_CLEANUP_DONE, std::to_wstring(removed)).c_str(),
                        LoadStr(IDS_APP_NAME).c_str(), MB_OK | MB_ICONINFORMATION);
            break;
        }
        case kCmdStartup:
            SetStartupEnabled(!IsStartupEnabled());
            break;
        case kCmdWebsite:
            ShellExecuteW(nullptr, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kCmdExit:
            restoreOnExit_ = true;
            restoreClosedOnExit_ = true;
            PostQuitMessage(0);
            break;
    }
}

void PumpFor(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) continue;  // already quitting
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    } while ((int)(end - GetTickCount()) > 0);
}

ComPtr<IFolderView2> ViewShowing(IWebBrowser2* browser, const std::wstring& folder) {
    ComPtr<IServiceProvider> sp;
    browser->QueryInterface(IID_PPV_ARGS(sp.Put()));
    ComPtr<IShellBrowser> sb;
    ComPtr<IShellView> sv;
    if (!sp || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(sb.Put()))) ||
        FAILED(sb->QueryActiveShellView(sv.Put())))
        return ComPtr<IFolderView2>();
    ComPtr<IFolderView2> view = sv.As<IFolderView2>();
    ComPtr<IPersistFolder2> pf;
    PIDLIST_ABSOLUTE pidl = nullptr;
    bool match = false;
    if (view && SUCCEEDED(view->GetFolder(IID_PPV_ARGS(pf.Put()))) && SUCCEEDED(pf->GetCurFolder(&pidl))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path))) {
            match = PathEqualsI(NormalizePath(path), folder);
            CoTaskMemFree(path);
        }
        CoTaskMemFree(pidl);
    }
    return match ? view : ComPtr<IFolderView2>();
}

// Identities of the browsers (windows and tabs) that are open now.
std::vector<ComPtr<IUnknown>> OpenBrowsers(IShellWindows* windows) {
    std::vector<ComPtr<IUnknown>> result;
    long count = 0;
    windows->get_Count(&count);
    for (long i = 0; i < count; i++) {
        VARIANT index;
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> disp;
        if (SUCCEEDED(windows->Item(index, disp.Put())) && disp) result.push_back(Identity(disp.Get()));
    }
    return result;
}

ComPtr<IFolderView2> WaitForView(IWebBrowser2* browser, const std::wstring& folder, int tenthsOfSecond) {
    ComPtr<IFolderView2> view;
    for (int i = 0; i < tenthsOfSecond && !view; i++) {
        PumpFor(100);
        view = ViewShowing(browser, folder);
    }
    return view;
}

// Opens `folder` in a new Explorer window that the user does not see. Returns the window and
// its view, or nothing when Explorer does not support hidden windows (Windows 11).
ComPtr<IWebBrowser2> OpenHiddenWindow(const std::wstring& folder, ComPtr<IFolderView2>* view) {
    ComPtr<IWebBrowser2> browser;
    HRESULT hr =
        CoCreateInstance(CLSID_ShellBrowserWindow, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(browser.Put()));
    if (FAILED(hr)) {
        LogLine(L"hidden window: create failed 0x%08x", hr);
        return ComPtr<IWebBrowser2>();
    }
    browser->put_Visible(VARIANT_FALSE);
    ComPtr<IShellBrowser> sb;
    if (ComPtr<IServiceProvider> sp = browser.As<IServiceProvider>())
        sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(sb.Put()));
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (sb && SUCCEEDED(SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr))) {
        hr = sb->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        CoTaskMemFree(pidl);
        if (SUCCEEDED(hr)) *view = WaitForView(browser.Get(), folder, 30);
    }
    if (!*view) {
        LogLine(L"hidden window: no view (browse 0x%08x)", hr);
        browser->Quit();
        return ComPtr<IWebBrowser2>();
    }
    return browser;
}

// Opens `folder` in a new minimized window that is not activated. `ownWindow` is false when
// Explorer put it in a tab of a window the user already had.
ComPtr<IWebBrowser2> OpenMinimizedWindow(const std::wstring& folder, ComPtr<IFolderView2>* view, bool* ownWindow) {
    *ownWindow = true;
    ComPtr<IWebBrowser2> browser;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.Put()))))
        return browser;
    auto before = OpenBrowsers(windows.Get());
    std::vector<HWND> beforeHwnds;
    for (auto& id : before) {
        SHANDLE_PTR h = 0;
        if (ComPtr<IWebBrowser2> b = id.As<IWebBrowser2>(); b && SUCCEEDED(b->get_HWND(&h))) beforeHwnds.push_back((HWND)h);
    }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + folder + L"\"").c_str(), nullptr, SW_SHOWMINNOACTIVE);
    for (int i = 0; i < 100 && !browser; i++) {
        PumpFor(100);
        for (auto& id : OpenBrowsers(windows.Get())) {
            if (std::any_of(before.begin(), before.end(), [&](const ComPtr<IUnknown>& b) { return b.Get() == id.Get(); }))
                continue;
            ComPtr<IWebBrowser2> candidate = id.As<IWebBrowser2>();
            if (!candidate) continue;
            if (ComPtr<IFolderView2> v = ViewShowing(candidate.Get(), folder)) {
                browser = candidate;
                *view = v;
                break;
            }
        }
    }
    if (browser) {
        SHANDLE_PTR h = 0;
        browser->get_HWND(&h);
        *ownWindow = std::find(beforeHwnds.begin(), beforeHwnds.end(), (HWND)h) == beforeHwnds.end();
    } else {
        LogLine(L"minimized window: not found");
    }
    return browser;
}

// Restores the grouping of `folder` in a window opened for it and closes the window again,
// which makes Explorer save the restored view state.
// Navigates `browser` to the exact ID list under which Explorer saved the view state.
ComPtr<IFolderView2> BrowseTo(IWebBrowser2* browser, const std::wstring& folder, PCIDLIST_ABSOLUTE idList) {
    ComPtr<IShellBrowser> sb;
    ComPtr<IServiceProvider> sp;
    browser->QueryInterface(IID_PPV_ARGS(sp.Put()));
    if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(sb.Put()));
    if (!sb) return ComPtr<IFolderView2>();
    // Leave the folder first so that the navigation creates a new view.
    PIDLIST_ABSOLUTE parent = ILClone(idList);
    if (parent && ILRemoveLastID(parent)) {
        sb->BrowseObject(parent, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        PumpFor(500);
    }
    CoTaskMemFree(parent);
    sb->BrowseObject(idList, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
    return WaitForView(browser, folder, 50);
}

// Opens the folder of `entry` through its saved ID list, restores the grouping and closes
// the window again, which makes Explorer save the restored view state.
void RestoreFolderInWindow(const GroupedEntry& entry) {
    const std::wstring& folder = entry.folder;
    ComPtr<IFolderView2> view;
    bool ownWindow = true;
    ComPtr<IWebBrowser2> browser = OpenHiddenWindow(folder, &view);
    if (!browser) browser = OpenMinimizedWindow(folder, &view, &ownWindow);
    if (!browser || !view) {
        LogLine(L"could not open %s to restore its grouping", folder.c_str());
        return;
    }
    if (!entry.idList.empty())
        view = BrowseTo(browser.Get(), folder, reinterpret_cast<PCIDLIST_ABSOLUTE>(entry.idList.data()));
    PROPERTYKEY current = {};
    BOOL ascending = TRUE;
    if (view && SUCCEEDED(view->GetGroupBy(&current, &ascending)) && IsPinKey(current)) {
        PROPERTYKEY key;
        ReadPreviousGroupBy(folder, &key, &ascending);
        view->SetGroupBy(key, ascending);
        PumpFor(300);
        LogLine(L"restored grouping of %s", folder.c_str());
    }
    if (ownWindow) browser->Quit();
    PumpFor(500);  // let Explorer save the view state before reporting the folder as done
    RegDeleteValueIn(HKEY_CURRENT_USER, kRegGroupedFolders, entry.valueName.c_str());
}

// Starts a program as the user of the desktop (not elevated), through Explorer.
// https://devblogs.microsoft.com/oldnewthing/20131118-00/?p=2643
bool ShellExecuteAsDesktopUser(const std::wstring& file, const std::wstring& args) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.Put()))))
        return false;
    VARIANT location, empty;
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VariantInit(&empty);
    long hwnd = 0;
    ComPtr<IDispatch> desktop;
    if (FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, desktop.Put())) ||
        !desktop)
        return false;
    ComPtr<IServiceProvider> sp = desktop.As<IServiceProvider>();
    ComPtr<IShellBrowser> sb;
    ComPtr<IShellView> sv;
    ComPtr<IDispatch> background;
    if (!sp || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(sb.Put()))) ||
        FAILED(sb->QueryActiveShellView(sv.Put())) ||
        FAILED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(background.Put()))))
        return false;
    ComPtr<IShellFolderViewDual> folderView = background.As<IShellFolderViewDual>();
    ComPtr<IDispatch> app;
    if (!folderView || FAILED(folderView->get_Application(app.Put()))) return false;
    ComPtr<IShellDispatch2> shell = app.As<IShellDispatch2>();
    if (!shell) return false;
    BSTR bstrFile = SysAllocString(file.c_str());
    VARIANT vArgs, vDir, vOperation, vShow;
    vArgs.vt = VT_BSTR;
    vArgs.bstrVal = SysAllocString(args.c_str());
    VariantInit(&vDir);
    VariantInit(&vOperation);
    vShow.vt = VT_I4;
    vShow.lVal = SW_SHOWNORMAL;
    HRESULT hr = shell->ShellExecute(bstrFile, vArgs, vDir, vOperation, vShow);
    SysFreeString(bstrFile);
    VariantClear(&vArgs);
    return SUCCEEDED(hr);
}

}  // namespace

void RestoreSavedGroupings() {
    if (GroupedEntries().empty()) return;
    // An elevated process (the uninstaller) would not reach the user's Explorer, so let a
    // copy of this program running as the desktop user do it, and wait for it.
    wchar_t force[8] = L"";
    bool delegate = IsUacElevated() || GetEnvironmentVariableW(L"EXPLORERPINNED_TEST_DELEGATE", force, 8) > 0;
    if (delegate) {
        SetEnvironmentVariableW(L"EXPLORERPINNED_TEST_DELEGATE", nullptr);
        if (!ShellExecuteAsDesktopUser(ExePath(), L"restore-groupings")) {
            LogLine(L"could not start the restore helper as the desktop user");
            return;
        }
        for (int i = 0; i < 120 && !GroupedEntries().empty(); i++) PumpFor(500);
        return;
    }
    auto entries = GroupedEntries();
    LogLine(L"restoring %zu saved grouping(s)", entries.size());
    for (const auto& entry : entries) {
        if (PathExists(entry.folder))
            RestoreFolderInWindow(entry);
        else
            RegDeleteValueIn(HKEY_CURRENT_USER, kRegGroupedFolders, entry.valueName.c_str());
    }
}

HWND FindAgentWindow() { return FindWindowW(kAgentWindowClass, nullptr); }

int RunAgent() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kAgentMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;  // another agent is already running in this session
    }
    HRESULT hr = OleInitialize(nullptr);
    int code;
    {
        Agent agent;
        code = agent.Run();
    }
    if (SUCCEEDED(hr)) OleUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}

bool EnsureAgentRunning() {
    if (FindAgentWindow()) return true;
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kAgentMutexName);
    if (mutex) {
        CloseHandle(mutex);
        return true;  // starting up
    }
    std::wstring cmd = L"\"" + ExePath() + L"\" agent";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace ep
