// End-to-end test: drives ExplorerPinned.exe against a real Explorer window and
// checks the resulting groups through UI Automation.
//
// Usage: ExplorerPinnedE2E.exe <path to ExplorerPinned.exe>
// Needs an interactive desktop with Explorer running and administrator rights
// (to register the property schema). Exit code = number of failed checks.

#include "common.h"

#include <exdisp.h>
#include <exdispid.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uiautomation.h>

#include <stdio.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "../src/resource.h"
#include "util.h"

using namespace ep;

namespace {

const PROPERTYKEY kItemNameDisplay = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 10};
const PROPERTYKEY kItemTypeText = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 4};
const PROPERTYKEY kDateModified = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 14};

int g_failures = 0;
std::wstring g_exe;
std::wstring g_dir;
std::wstring g_pinnedLabel;

void Print(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    fputs(out.c_str(), stdout);
    fputs("\n", stdout);
    fflush(stdout);
}

void Check(bool ok, const std::wstring& what, const std::wstring& detail = L"") {
    Print(std::wstring(ok ? L"PASS " : L"FAIL ") + what + (detail.empty() ? L"" : L"  [" + detail + L"]"));
    if (!ok) g_failures++;
}

void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    } while ((int)(end - GetTickCount()) > 0);
}

bool WaitFor(const std::function<bool()>& cond, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        if (cond()) return true;
        if (GetTickCount() - start > timeoutMs) return false;
        Pump(300);
    }
}

int RunExe(const std::wstring& args, DWORD timeoutMs = 60000) {
    std::wstring cmd = L"\"" + g_exe + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    DWORD start = GetTickCount();
    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && GetTickCount() - start < timeoutMs) Pump(50);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Runs the program and returns what it printed.
std::wstring RunExeOutput(const std::wstring& args) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + g_exe + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi = {};
    std::string out;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(writePipe);
        writePipe = nullptr;
        char buf[4096];
        DWORD n;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (writePipe) CloseHandle(writePipe);
    CloseHandle(readPipe);
    int len = MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), w.data(), len);
    return w;
}

// Lists the property schema files registered with Windows that belong to this program.
std::wstring RegisteredSchemas() {
    std::wstring result;
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertySchema",
                      0, KEY_READ, &root) != ERROR_SUCCESS)
        return L"(no key)";
    wchar_t sub[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t name[256], data[1024];
        for (DWORD j = 0;; j++) {
            DWORD nl = 256, dl = sizeof(data), type = 0;
            if (RegEnumValueW(k, j, name, &nl, nullptr, &type, (BYTE*)data, &dl) != ERROR_SUCCESS) break;
            if (type == REG_SZ && wcsstr(data, L"ExplorerPinned")) result += std::wstring(sub) + L"\\" + name + L"=" + data + L"; ";
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return result.empty() ? L"(none)" : result;
}

void TouchFile(const std::wstring& path, int year) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, "test", 4, &w, nullptr);
    SYSTEMTIME st = {};
    st.wYear = (WORD)year;
    st.wMonth = 6;
    st.wDay = 1;
    st.wHour = 12;
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) s += (s.empty() ? L"" : L", ") + x;
    return s;
}

// ---------------------------------------------------------------- UI Automation
struct Groups {
    std::vector<std::wstring> order;                         // group names, top to bottom
    std::map<std::wstring, std::set<std::wstring>> members;  // group -> item names
    std::wstring Describe() const {
        std::wstring s;
        for (auto& g : order) {
            std::vector<std::wstring> items(members.at(g).begin(), members.at(g).end());
            s += L"{" + g + L": " + Join(items) + L"} ";
        }
        return s;
    }
    std::set<std::wstring> First() const { return order.empty() ? std::set<std::wstring>() : members.at(order[0]); }
};

HWND g_uiaHwnd;
Groups g_uiaResult;

DWORD WINAPI UiaThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Groups result;
    IUIAutomation* uia = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    IUIAutomationElement* root = nullptr;
    if (uia) uia->ElementFromHandle(g_uiaHwnd, &root);
    if (root) {
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = UIA_ListItemControlTypeId;
        IUIAutomationCondition* cond = nullptr;
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
        IUIAutomationElementArray* arr = nullptr;
        root->FindAll(TreeScope_Descendants, cond, &arr);
        IUIAutomationTreeWalker* walker = nullptr;
        uia->get_ControlViewWalker(&walker);
        int len = 0;
        if (arr) arr->get_Length(&len);
        std::map<std::wstring, int> top;
        for (int i = 0; i < len; i++) {
            IUIAutomationElement* el = nullptr;
            arr->GetElement(i, &el);
            BSTR name = nullptr, groupName = nullptr;
            el->get_CurrentName(&name);
            IUIAutomationElement* parent = nullptr;
            walker->GetParentElement(el, &parent);
            CONTROLTYPEID type = 0;
            if (parent) {
                parent->get_CurrentControlType(&type);
                if (type == UIA_GroupControlTypeId) parent->get_CurrentName(&groupName);
            }
            RECT r = {};
            el->get_CurrentBoundingRectangle(&r);
            std::wstring g = groupName ? groupName : L"(none)";
            if (!top.count(g) || r.top < top[g]) top[g] = r.top;
            result.members[g].insert(name ? name : L"");
            SysFreeString(name);
            SysFreeString(groupName);
            if (parent) parent->Release();
            el->Release();
        }
        std::vector<std::pair<int, std::wstring>> order;
        for (auto& t : top) order.push_back({t.second, t.first});
        std::sort(order.begin(), order.end());
        for (auto& o : order) result.order.push_back(o.second);
        if (arr) arr->Release();
        if (walker) walker->Release();
        cond->Release();
        root->Release();
    }
    if (uia) uia->Release();
    g_uiaResult = result;
    CoUninitialize();
    return 0;
}

// UIA calls into Explorer are made from a worker thread while this thread keeps pumping.
Groups ReadGroups(HWND hwnd) {
    g_uiaHwnd = hwnd;
    HANDLE t = CreateThread(nullptr, 0, UiaThread, nullptr, 0, nullptr);
    DWORD start = GetTickCount();
    while (WaitForSingleObject(t, 0) == WAIT_TIMEOUT && GetTickCount() - start < 30000) Pump(20);
    CloseHandle(t);
    return g_uiaResult;
}

// ---------------------------------------------------------------- Explorer window
struct Window {
    IWebBrowser2* browser = nullptr;
    HWND hwnd = nullptr;
    IFolderView2* view = nullptr;

    bool Refresh() {
        if (view) view->Release();
        view = nullptr;
        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        IShellView* sv = nullptr;
        browser->QueryInterface(IID_PPV_ARGS(&sp));
        if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb));
        if (sb) sb->QueryActiveShellView(&sv);
        if (sv) sv->QueryInterface(IID_PPV_ARGS(&view));
        if (sv) sv->Release();
        if (sb) sb->Release();
        if (sp) sp->Release();
        return view != nullptr;
    }
    std::wstring Path() {
        std::wstring result;
        IPersistFolder2* pf = nullptr;
        if (view && SUCCEEDED(view->GetFolder(IID_PPV_ARGS(&pf)))) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pf->GetCurFolder(&pidl))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &p))) {
                    result = p;
                    CoTaskMemFree(p);
                }
                CoTaskMemFree(pidl);
            }
            pf->Release();
        }
        return result;
    }
    PROPERTYKEY GroupBy() {
        PROPERTYKEY k = {};
        BOOL asc = TRUE;
        if (Refresh()) view->GetGroupBy(&k, &asc);
        return k;
    }
    void Navigate(const std::wstring& path) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        browser->QueryInterface(IID_PPV_ARGS(&sp));
        if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb));
        if (sb) sb->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        if (sb) sb->Release();
        if (sp) sp->Release();
        CoTaskMemFree(pidl);
        WaitFor([&] { return Refresh() && _wcsicmp(Path().c_str(), path.c_str()) == 0; }, 10000);
        Pump(1000);
    }
};

bool OpenWindow(Window* w) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", g_dir.c_str(), nullptr, SW_SHOWNORMAL);
    return WaitFor(
        [&] {
            IShellWindows* sw = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sw)))) return false;
            long count = 0;
            sw->get_Count(&count);
            bool found = false;
            for (long i = 0; i < count && !found; i++) {
                VARIANT index;
                index.vt = VT_I4;
                index.lVal = i;
                IDispatch* disp = nullptr;
                if (FAILED(sw->Item(index, &disp)) || !disp) continue;
                IWebBrowser2* browser = nullptr;
                disp->QueryInterface(IID_PPV_ARGS(&browser));
                disp->Release();
                if (!browser) continue;
                w->browser = browser;
                if (w->Refresh() && _wcsicmp(w->Path().c_str(), g_dir.c_str()) == 0) {
                    SHANDLE_PTR h = 0;
                    browser->get_HWND(&h);
                    w->hwnd = (HWND)h;
                    found = true;
                } else {
                    browser->Release();
                    w->browser = nullptr;
                }
            }
            sw->Release();
            return found;
        },
        15000);
}

// Waits until the topmost group is the pinned group with exactly `expected` items.
bool ExpectPinned(Window& w, const std::set<std::wstring>& expected, const std::wstring& what) {
    Groups last;
    bool ok = WaitFor(
        [&] {
            last = ReadGroups(w.hwnd);
            return !last.order.empty() && last.order[0] == g_pinnedLabel && last.First() == expected;
        },
        15000);
    std::vector<std::wstring> exp(expected.begin(), expected.end());
    Check(ok, what, L"expected pinned {" + Join(exp) + L"}, got " + last.Describe());
    return ok;
}

bool ExpectNoPinnedGroup(Window& w, const std::wstring& what) {
    Groups last;
    bool ok = WaitFor(
        [&] {
            last = ReadGroups(w.hwnd);
            return std::find(last.order.begin(), last.order.end(), g_pinnedLabel) == last.order.end() &&
                   !IsEqualPropertyKey(w.GroupBy(), kPinStateKeys[0]) && !IsEqualPropertyKey(w.GroupBy(), kPinStateKeys[1]);
        },
        15000);
    Check(ok, what, last.Describe());
    return ok;
}

// ---------------------------------------------------------------- context menu
std::vector<std::wstring> MenuItems(const std::wstring& path) {
    std::vector<std::wstring> items;
    IShellItem* si = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&si)))) return items;
    IContextMenu* cm = nullptr;
    if (SUCCEEDED(si->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm)))) {
        HMENU m = CreatePopupMenu();
        cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
        for (int i = 0; i < GetMenuItemCount(m); i++) {
            wchar_t t[512] = L"";
            GetMenuStringW(m, i, t, 512, MF_BYPOSITION);
            if (t[0]) items.push_back(t);
        }
        DestroyMenu(m);
        cm->Release();
    }
    si->Release();
    return items;
}

int KeyIndexOf(const PROPERTYKEY& k) {
    for (int i = 0; i < 2; i++)
        if (IsEqualPropertyKey(k, kPinStateKeys[i])) return i;
    return -1;
}

bool Contains(const std::vector<std::wstring>& v, const std::wstring& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

constexpr LANGID kJapanese = MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN);
constexpr LANGID kEnglish = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

std::wstring ExeString(UINT id, LANGID language) {
    HMODULE m = LoadLibraryExW(g_exe.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    std::wstring s = m ? LoadStrLang(m, id, language) : std::wstring();
    if (m) FreeLibrary(m);
    return s;
}

std::wstring SchemaLabel() {
    PSRefreshPropertySchema();
    IPropertyDescription* desc = nullptr;
    std::wstring label;
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

// Saves the window as a 24-bit BMP (used for the README screenshot).
void SaveScreenshot(HWND hwnd, const std::wstring& path) {
    RECT r;
    GetWindowRect(hwnd, &r);
    int width = r.right - r.left, height = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) BitBlt(mem, 0, 0, width, height, screen, r.left, r.top, SRCCOPY);
    SelectObject(mem, old);
    BITMAPINFOHEADER bi = {sizeof(bi)};
    bi.biWidth = width;
    bi.biHeight = height;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    DWORD stride = ((width * 3 + 3) & ~3);
    std::vector<BYTE> pixels(stride * height);
    GetDIBits(mem, bmp, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    BITMAPFILEHEADER bf = {};
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    bf.bfSize = bf.bfOffBits + (DWORD)pixels.size();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(f, &bf, sizeof(bf), &written, nullptr);
        WriteFile(f, &bi, sizeof(bi), &written, nullptr);
        WriteFile(f, pixels.data(), (DWORD)pixels.size(), &written, nullptr);
        CloseHandle(f);
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

bool IsPinnedInRegistry(const std::wstring& path) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPins, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, path.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        Print(L"usage: ExplorerPinnedE2E <ExplorerPinned.exe> [screenshot directory]");
        return 100;
    }
    std::wstring shots = argc > 2 ? argv[2] : L"";
    wchar_t full[MAX_PATH];
    GetFullPathNameW(argv[1], MAX_PATH, full, nullptr);
    g_exe = full;
    OleInitialize(nullptr);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetLongPathNameW(tmp, tmp, MAX_PATH);
    g_dir = std::wstring(tmp) + L"ep_e2e";
    SHCreateDirectoryExW(nullptr, g_dir.c_str(), nullptr);
    const std::wstring special = L"テスト (1) & x.txt";
    TouchFile(g_dir + L"\\alpha.txt", 2019);
    TouchFile(g_dir + L"\\bravo.txt", 2021);
    TouchFile(g_dir + L"\\charlie.txt", 2023);
    TouchFile(g_dir + L"\\zulu.exe", 2020);
    TouchFile(g_dir + L"\\" + special, 2022);
    CreateDirectoryW((g_dir + L"\\delta").c_str(), nullptr);
    DeleteFileW((g_dir + L"\\echo.txt").c_str());

    // --- resources
    Check(ExeString(IDS_MENU_PIN_FILE, kJapanese) == L"このファイルをピン止めする", L"Japanese string table");
    Check(ExeString(IDS_MENU_PIN_FILE, kEnglish) == L"Pin this file to the top", L"English string table");

    // --- setup (in Japanese, like the user's machine)
    RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Language", L"ja");
    Check(RunExe(L"setup --no-startup --no-agent --quiet") == 0, L"setup exits with 0");
    PSRefreshPropertySchema();
    for (int i = 0; i < 2; i++) {
        IPropertyDescription* desc = nullptr;
        HRESULT hr = PSGetPropertyDescription(kPinStateKeys[i], IID_PPV_ARGS(&desc));
        if (desc) desc->Release();
        Check(SUCCEEDED(hr), L"property " + std::to_wstring(i) + L" registered");
    }
    g_pinnedLabel = SchemaLabel();
    Check(g_pinnedLabel == L"ピン止め", L"group label is Japanese", g_pinnedLabel);
    std::wstring pinLabel = ExeString(IDS_MENU_PIN_FILE, kJapanese);
    std::wstring pinFolderLabel = ExeString(IDS_MENU_PIN_FOLDER, kJapanese);
    std::wstring unpinLabel = ExeString(IDS_MENU_UNPIN, kJapanese);
    {
        auto items = MenuItems(g_dir + L"\\alpha.txt");
        Check(Contains(items, pinLabel) && !Contains(items, unpinLabel), L"menu on a file before pinning", Join(items));
        auto folder = MenuItems(g_dir + L"\\delta");
        Check(Contains(folder, pinFolderLabel), L"menu on a folder before pinning", Join(folder));
    }

    // --- agent
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION agent = {};
    std::wstring cmd = L"\"" + g_exe + L"\" agent";
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &agent);
    Check(WaitFor([] { return FindWindowW(kAgentWindowClass, nullptr) != nullptr; }, 10000), L"agent starts");
    Check(RunExe(L"agent") == 0 && FindWindowW(kAgentWindowClass, nullptr), L"second agent exits immediately");

    // --- pin before the folder is opened
    Check(RunExe(L"pin \"" + g_dir + L"\\bravo.txt\" \"" + g_dir + L"\\delta\"") == 0, L"pin command");
    Check(IsPinnedInRegistry(g_dir + L"\\bravo.txt") && IsPinnedInRegistry(g_dir + L"\\delta"), L"pins stored");

    Window w;
    if (!OpenWindow(&w)) {
        Check(false, L"open Explorer window");
        return 1;
    }
    w.view->SetCurrentViewMode(FVM_DETAILS);
    SORTCOLUMN byName = {kItemNameDisplay, SORT_ASCENDING};
    w.view->SetSortColumns(&byName, 1);
    ExpectPinned(w, {L"bravo", L"delta"}, L"pinned group on top when the folder opens (sorted by name)");

    SORTCOLUMN byDate = {kDateModified, SORT_DESCENDING};
    w.Refresh();
    w.view->SetSortColumns(&byDate, 1);
    Pump(1500);
    ExpectPinned(w, {L"bravo", L"delta"}, L"pinned group stays on top when sorted by date (newest first)");
    if (!shots.empty()) {
        SetForegroundWindow(w.hwnd);
        Pump(800);
        SaveScreenshot(w.hwnd, shots + L"\\sorted-by-date.bmp");
    }
    SORTCOLUMN byNameDesc = {kItemNameDisplay, SORT_DESCENDING};
    w.Refresh();
    w.view->SetSortColumns(&byNameDesc, 1);
    Pump(1500);
    ExpectPinned(w, {L"bravo", L"delta"}, L"pinned group stays on top when sorted by name descending");

    // --- changes while the folder is open
    RunExe(L"unpin \"" + g_dir + L"\\bravo.txt\"");
    ExpectPinned(w, {L"delta"}, L"unpin moves the item out of the pinned group");
    RunExe(L"pin \"" + g_dir + L"\\" + special + L"\"");
    ExpectPinned(w, {L"delta", L"テスト (1) & x"}, L"pin with special characters");

    TouchFile(g_dir + L"\\echo.txt", 2025);
    Pump(2500);
    {
        Groups g = ReadGroups(w.hwnd);
        Check(!g.First().count(L"echo") && !g.order.empty() && g.order[0] == g_pinnedLabel,
              L"new file is not pinned", g.Describe());
    }
    RunExe(L"toggle \"" + g_dir + L"\\echo.txt\"");
    ExpectPinned(w, {L"delta", L"テスト (1) & x", L"echo"}, L"toggle pins a new file");

    {
        IShellView* sv = nullptr;
        w.Refresh();
        if (SUCCEEDED(w.view->QueryInterface(IID_PPV_ARGS(&sv)))) {
            sv->Refresh();
            sv->Release();
        }
        Pump(1000);
        ExpectPinned(w, {L"delta", L"テスト (1) & x", L"echo"}, L"pinned group comes back after F5 (refresh)");
    }

    // --- context menu
    {
        auto file = MenuItems(g_dir + L"\\alpha.txt");
        Check(Contains(file, pinLabel) && !Contains(file, unpinLabel), L"menu on an unpinned file", Join(file));
        auto pinned = MenuItems(g_dir + L"\\echo.txt");
        Check(Contains(pinned, unpinLabel) && !Contains(pinned, pinLabel), L"menu on a pinned file", Join(pinned));
        auto folder = MenuItems(g_dir + L"\\delta");
        Check(Contains(folder, unpinLabel) && !Contains(folder, pinFolderLabel), L"menu on a pinned folder",
              Join(folder));
        auto special2 = MenuItems(g_dir + L"\\" + special);
        Check(Contains(special2, unpinLabel), L"menu on a pinned file with special characters", Join(special2));
    }

    // --- navigation
    w.Navigate(L"C:\\Windows");
    w.Navigate(g_dir);
    ExpectPinned(w, {L"delta", L"テスト (1) & x", L"echo"}, L"pinned group after navigating away and back");

    // --- the user picks another grouping: respected until the folder is opened again
    w.Refresh();
    w.view->SetGroupBy(kItemTypeText, TRUE);
    Pump(5000);
    Check(IsEqualPropertyKey(w.GroupBy(), kItemTypeText), L"user-selected grouping is kept in the open view");
    w.Navigate(L"C:\\Windows");
    w.Navigate(g_dir);
    ExpectPinned(w, {L"delta", L"テスト (1) & x", L"echo"}, L"pinned group is applied again on the next visit");

    // --- removing all pins restores the original grouping
    RunExe(L"unpin \"" + g_dir + L"\\delta\" \"" + g_dir + L"\\" + special + L"\" \"" + g_dir + L"\\echo.txt\"");
    ExpectNoPinnedGroup(w, L"grouping is removed when the folder has no pins");
    {
        auto file = MenuItems(g_dir + L"\\echo.txt");
        Check(Contains(file, pinLabel) && !Contains(file, unpinLabel), L"menu after unpinning everything", Join(file));
    }

    // --- a pin whose item is gone does not keep the folder grouped
    RunExe(L"pin \"" + g_dir + L"\\charlie.txt\"");
    ExpectPinned(w, {L"charlie"}, L"pin again");
    DeleteFileW((g_dir + L"\\charlie.txt").c_str());
    ExpectNoPinnedGroup(w, L"grouping is removed when the pinned item is deleted");
    TouchFile(g_dir + L"\\charlie.txt", 2023);
    ExpectPinned(w, {L"charlie"}, L"pinned group returns when the item comes back");
    Check(RunExe(L"list") == 0, L"list command");

    // --- multiple selection: Explorer starts one "pin" process per item at the same time
    {
        std::wstring multi = g_dir + L"\\multi";
        CreateDirectoryW(multi.c_str(), nullptr);
        std::vector<HANDLE> procs;
        for (int i = 1; i <= 6; i++) {
            std::wstring file = multi + L"\\m" + std::to_wstring(i) + L".txt";
            TouchFile(file, 2024);
            std::wstring cmdline = L"\"" + g_exe + L"\" pin \"" + file + L"\"";
            STARTUPINFOW s2 = {sizeof(s2)};
            PROCESS_INFORMATION p2 = {};
            if (CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &s2, &p2)) {
                CloseHandle(p2.hThread);
                procs.push_back(p2.hProcess);
            }
        }
        WaitForMultipleObjects((DWORD)procs.size(), procs.data(), TRUE, 30000);
        for (HANDLE h : procs) CloseHandle(h);
        bool allStored = true;
        for (int i = 1; i <= 6; i++) allStored &= IsPinnedInRegistry(multi + L"\\m" + std::to_wstring(i) + L".txt");
        Check(allStored, L"concurrent pin commands store every pin");
        bool menusOk = WaitFor(
            [&] {
                for (int i = 1; i <= 6; i++)
                    if (!Contains(MenuItems(multi + L"\\m" + std::to_wstring(i) + L".txt"), unpinLabel)) return false;
                return true;
            },
            10000);
        Check(menusOk, L"menus are in sync after concurrent pin commands");
        std::wstring args = L"unpin";
        for (int i = 1; i <= 6; i++) args += L" \"" + multi + L"\\m" + std::to_wstring(i) + L".txt\"";
        RunExe(args);
    }

    // --- the Downloads folder (grouped by date by default on client editions)
    {
        PWSTR dl = nullptr;
        SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl);
        std::wstring downloads = dl ? dl : L"";
        CoTaskMemFree(dl);
        std::wstring file = downloads + L"\\ep_e2e_download.txt";
        TouchFile(file, 2024);
        w.Navigate(downloads);
        PROPERTYKEY before = w.GroupBy();
        wchar_t desc[96];
        swprintf_s(desc, L"group-by before: pid %lu", before.pid);
        Print(desc);
        RunExe(L"pin \"" + file + L"\"");
        ExpectPinned(w, {L"ep_e2e_download"}, L"Downloads: pinned group on top");
        RunExe(L"unpin \"" + file + L"\"");
        Check(WaitFor([&] { return IsEqualPropertyKey(w.GroupBy(), before); }, 15000),
              L"Downloads: original grouping is restored after unpinning");
        DeleteFileW(file.c_str());
        w.Navigate(g_dir);
    }

    // --- the schema can be re-registered in another language
    Print(L"schemas before: " + RegisteredSchemas());
    Check(RunExe(L"register-schema --lang en") == 0, L"register-schema in English");
    Print(L"schemas after: " + RegisteredSchemas());
    {
        std::wstring status = RunExeOutput(L"status");
        Check(status.find(L"registered (Pinned)") != std::wstring::npos, L"English group label", status);
    }

    // --- a folder that is not open when the agent exits keeps Explorer's saved "Pinned" grouping
    std::wstring other = g_dir + L"\\other";
    CreateDirectoryW(other.c_str(), nullptr);
    TouchFile(other + L"\\o1.txt", 2020);
    TouchFile(other + L"\\o2.txt", 2021);
    RunExe(L"pin \"" + other + L"\\o1.txt\"");
    w.Navigate(other);
    ExpectPinned(w, {L"o1"}, L"second folder is grouped");
    w.Navigate(g_dir);

    // --- exit restores the grouping; uninstall cleans up
    Check(RunExe(L"exit") == 0, L"exit command exits with 0");
    Check(WaitFor([] { return FindWindowW(kAgentWindowClass, nullptr) == nullptr; }, 10000), L"exit stops the agent");
    ExpectNoPinnedGroup(w, L"exit restores the original grouping");
    w.Navigate(other);
    ExpectNoPinnedGroup(w, L"exit restores the grouping of folders that were not open");
    w.Navigate(g_dir);
    // --- uninstall restores the saved grouping of folders that are not open; when the
    // uninstaller is elevated this runs as the desktop user (forced here for the test)
    std::wstring third = g_dir + L"\\third";
    CreateDirectoryW(third.c_str(), nullptr);
    TouchFile(third + L"\\t1.txt", 2020);
    TouchFile(third + L"\\t2.txt", 2021);
    w.Navigate(third);
    w.Refresh();
    w.view->SetGroupBy(kPinStateKeys[0], TRUE);
    Pump(1000);
    RegWriteString(HKEY_CURRENT_USER, kRegPreviousGroupBy, third.c_str(), L"{00000000-0000-0000-0000-000000000000},0,1");
    w.Navigate(g_dir);
    SetEnvironmentVariableW(L"EXPLORERPINNED_TEST_DELEGATE", L"1");
    Check(RunExe(L"uninstall --quiet") == 0, L"uninstall exits with 0");
    SetEnvironmentVariableW(L"EXPLORERPINNED_TEST_DELEGATE", nullptr);
    w.Navigate(third);
    {
        PROPERTYKEY k = w.GroupBy();
        Check(KeyIndexOf(k) < 0, L"uninstall restores the grouping of folders that were not open",
              L"group-by pid " + std::to_wstring(k.pid));
    }
    w.Navigate(g_dir);
    Print(L"schemas after uninstall: " + RegisteredSchemas());
    {
        std::wstring status = RunExeOutput(L"status");
        Check(status.find(L"schema: not registered") != std::wstring::npos, L"uninstall unregisters the property",
              status);
        HKEY k;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, kRegRoot, 0, KEY_READ, &k) != ERROR_SUCCESS, L"uninstall removes settings");
        auto file = MenuItems(g_dir + L"\\alpha.txt");
        Check(!Contains(file, pinLabel), L"uninstall removes the menu", Join(file));
    }

    w.browser->Quit();
    if (agent.hProcess) {
        CloseHandle(agent.hProcess);
        CloseHandle(agent.hThread);
    }
    Print(g_failures ? L"E2E FAILED: " + std::to_wstring(g_failures) + L" check(s)" : L"E2E PASSED");
    OleUninitialize();
    return g_failures;
}
