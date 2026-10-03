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
#include <set>

#include "comutil.h"
#include "pinstore.h"
#include "util.h"

namespace ep {
namespace {

// {432E90E6-6BCF-44FE-9F87-8BA191F04870}
const CLSID kOverlayClsid = {0x432e90e6, 0x6bcf, 0x44fe, {0x9f, 0x87, 0x8b, 0xa1, 0x91, 0xf0, 0x48, 0x70}};
constexpr wchar_t kOverlayClsidString[] = L"{432E90E6-6BCF-44FE-9F87-8BA191F04870}";
// Windows loads the overlay handlers in the order of these names and uses only the first
// 15 overlays. The leading spaces get this handler loaded before the others; it takes no
// overlay slot (GetOverlayInfo fails), so the overlays of other programs are not affected.
constexpr wchar_t kOverlayKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\    ExplorerPinned";

constexpr UINT kGetShellBrowser = WM_USER + 7;  // WM_GETISHELLBROWSER, answered by shell browser windows
constexpr DWORD kPollMs = 700;
constexpr DWORD kNewViewDelayMs = 600;
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
    std::wstring folderPath;
    NameSet applied[2];
    NameSet lastNames;
    bool grouped = false;  // the pinned grouping was applied to this view
    DWORD lastRegroup = 0;
    int attempts = 0;
    bool gaveUp = false;
};

thread_local std::map<HWND, Dialog>* t_dialogs = nullptr;

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

// The dialog is about to close (or to leave the folder for a typed path): put the original
// grouping back first, since the dialog saves its view state for the folder.
void BeforeClose(HWND dialog) {
    if (!t_dialogs) return;
    auto it = t_dialogs->find(dialog);
    if (it == t_dialogs->end() || !it->second.grouped || GetEnvironmentVariableW(L"EXPLORERPINNED_NO_RESTORE", nullptr, 0))
        return;
    ComPtr<IFolderView2> view = ViewOf(dialog);
    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    if (view && SUCCEEDED(view->GetGroupBy(&groupKey, &ascending)) && IsPinKey(groupKey)) RestoreGrouping(it->second, view.Get());
    // If the dialog stays open, group the view again.
    it->second.attempts = 0;
    it->second.lastRegroup = 0;
}

bool IsCloseCommand(UINT message, WPARAM wp) {
    return (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) || message == WM_CLOSE ||
           (message == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_CLOSE);
}

// Runs on the dialog's thread.
void ProcessDialog(HWND dialog) {
    ComPtr<IFolderView2> view = ViewOf(dialog);
    if (!view) return;
    if (!t_dialogs) t_dialogs = new std::map<HWND, Dialog>();
    if (!t_dialogs->count(dialog)) {
        // Research: which interfaces the dialog's browser offers (for navigation events).
        HWND defView = FindDefView(dialog);
        ComPtr<IShellBrowser> sb = defView ? ShellBrowserOf(dialog, defView) : ComPtr<IShellBrowser>();
        ComPtr<IServiceProvider> sp = sb ? sb.As<IServiceProvider>() : ComPtr<IServiceProvider>();
        ComPtr<IExplorerBrowser> eb;
        ComPtr<IFileDialog> fd;
        ComPtr<IExplorerBrowser> ebService;
        ComPtr<IFileDialog> fdService;
        if (sb) {
            sb->QueryInterface(IID_PPV_ARGS(eb.Put()));
            sb->QueryInterface(IID_PPV_ARGS(fd.Put()));
        }
        if (sp) {
            sp->QueryService(SID_SExplorerBrowserFrame, IID_PPV_ARGS(fdService.Put()));
            sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(ebService.Put()));
        }
        LogLine(L"dialog: browser %d, IExplorerBrowser %d/%d, IFileDialog %d/%d", sb ? 1 : 0, eb ? 1 : 0,
                ebService ? 1 : 0, fd ? 1 : 0, fdService ? 1 : 0);
    }
    Dialog& d = (*t_dialogs)[dialog];
    DWORD now = GetTickCount();

    ComPtr<IUnknown> identity = Identity(view.Get());
    if (identity.Get() != d.viewIdentity.Get()) {
        // A new folder: Explorer fills the view and applies its saved view state first.
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
        return;
    }
    if (d.folderPath.empty() || now - d.seen < kNewViewDelayMs) return;

    ComPtr<IShellFolder> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(folder.Put())))) return;
    NameSet names;
    for (const auto& n : PinStore::NamesInFolder(PinStore::LoadAll(), d.folderPath)) {
        if (PITEMID_CHILD child = ParseChild(folder.Get(), n)) {
            names.insert(n);
            CoTaskMemFree(child);
        }
    }
    PROPERTYKEY groupKey = {};
    BOOL ascending = TRUE;
    if (FAILED(view->GetGroupBy(&groupKey, &ascending))) return;
    int current = KeyIndex(groupKey);

    if (names.empty()) {
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
    if (now - d.lastRegroup < 1000) return;

    int key = current < 0 ? 0 : 1 - current;
    if (current < 0 && !IsPinKey(groupKey)) SavePreviousGrouping(d.folderPath, groupKey, ascending);
    if (!WriteKey(d, view.Get(), folder.Get(), key, names)) return;  // items not in the view yet
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
        } else if (IsCloseCommand(msg->message, msg->wParam)) {
            BeforeClose(msg->hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Sent messages: the buttons, Enter and Esc reach the dialog as a sent WM_COMMAND.
LRESULT CALLBACK CallWndProcHook(int code, WPARAM wp, LPARAM lp) {
    const CWPSTRUCT* msg = reinterpret_cast<const CWPSTRUCT*>(lp);
    if (code == HC_ACTION && IsCloseCommand(msg->message, msg->wParam)) BeforeClose(msg->hwnd);
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::set<DWORD>* g_hookedThreads = nullptr;  // worker thread only

BOOL CALLBACK VisitWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[16];
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"#32770") != 0) return TRUE;
    if (!FindDefView(hwnd)) return TRUE;
    if (!g_hookedThreads->count(tid)) {
        // A hook on the dialog's thread lets the work run there, where the view lives.
        if (!SetWindowsHookExW(WH_GETMESSAGE, GetMessageHook, g_module, tid)) return TRUE;
        SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcHook, g_module, tid);
        g_hookedThreads->insert(tid);
    }
    PostMessageW(hwnd, g_tickMessage, 0, 0);
    return TRUE;
}

DWORD WINAPI Worker(void*) {
    g_hookedThreads = new std::set<DWORD>();
    for (;;) {
        Sleep(kPollMs);
        EnumWindows(VisitWindow, 0);
    }
}

bool DialogsEnabled() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = FileNamePart(exe);
    // Explorer windows are handled by the agent.
    if (PathEqualsI(name, L"explorer.exe")) return false;
    if (GetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", nullptr, 0) > 0) return false;
    std::wstring setting;
    if (RegReadString(HKEY_CURRENT_USER, kRegRoot, L"Dialogs", &setting) && setting == L"0") return false;
    return true;
}

void StartOnce() {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1)) return;
    if (!DialogsEnabled()) return;
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

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kOverlayClsidString;
    bool ok = RegWriteString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"Explorer Pinned (file dialogs)") &&
              RegWriteString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, path) &&
              RegWriteString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              RegWriteString(HKEY_LOCAL_MACHINE, kOverlayKey, nullptr, kOverlayClsidString);
    RegWriteString(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                   kOverlayClsidString, L"Explorer Pinned (file dialogs)");
    return ok ? S_OK : E_ACCESSDENIED;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kOverlayKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"Software\\Classes\\CLSID\\") + kOverlayClsidString).c_str());
    RegDeleteValueIn(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                     kOverlayClsidString);
    return S_OK;
}
