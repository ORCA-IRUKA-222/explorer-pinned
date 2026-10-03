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
#include <memory>
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

bool OpenWindow(Window* w, const std::wstring& path = g_dir) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", path.c_str(), nullptr, SW_SHOWNORMAL);
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
                if (w->Refresh() && _wcsicmp(w->Path().c_str(), path.c_str()) == 0) {
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

// The group label depends on the language the property was registered in (the test
// re-registers it in English near the end; open windows may still show the old label).
bool IsPinnedLabel(const std::wstring& name) {
    return name == g_pinnedLabel || name == L"ピン止め" || name == L"Pinned";
}

// Waits until the topmost group is the pinned group with exactly `expected` items.
bool ExpectPinned(Window& w, const std::set<std::wstring>& expected, const std::wstring& what) {
    Groups last;
    bool ok = WaitFor(
        [&] {
            last = ReadGroups(w.hwnd);
            return !last.order.empty() && IsPinnedLabel(last.order[0]) && last.First() == expected;
        },
        15000);
    std::vector<std::wstring> exp(expected.begin(), expected.end());
    Check(ok, what, L"expected pinned {" + Join(exp) + L"}, got " + last.Describe());
    return ok;
}

// Waits until Explorer lists the items of a freshly filled folder. Right after it is
// created, a large folder can keep Explorer busy (icons, virus scan) for a while, and the
// agent cannot group a view that shows nothing yet.
void WaitForItems(Window& w, const std::wstring& what) {
    DWORD start = GetTickCount();
    bool ok = WaitFor([&] { return !ReadGroups(w.hwnd).order.empty(); }, 60000);
    Print(what + (ok ? L": Explorer lists the items after " : L": Explorer lists no items after ") +
          std::to_wstring((GetTickCount() - start) / 1000) + L" s");
}

bool ExpectNoPinnedGroup(Window& w, const std::wstring& what) {
    Groups last;
    bool ok = WaitFor(
        [&] {
            last = ReadGroups(w.hwnd);
            return std::none_of(last.order.begin(), last.order.end(), IsPinnedLabel) &&
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

// The schema file of version 1.0.x (ExplorerPinned.propdesc with the old property keys).
void WriteLegacySchema(const std::wstring& path) {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"utf-16\"?>\r\n"
        L"<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
        L"  <propertyDescriptionList publisher=\"ExplorerPinned\" product=\"ExplorerPinned\">\r\n";
    for (int i = 0; i < 2; i++) {
        xml += std::wstring(L"    <propertyDescription name=\"") + kLegacyPinStateNames[i] + L"\" formatID=\"" +
               kLegacyPinStateFormatId + L"\" propID=\"" + std::to_wstring(kLegacyPinStateKeys[i].pid) + L"\">\r\n"
               L"      <searchInfo inInvertedIndex=\"false\" isColumn=\"false\"/>\r\n"
               L"      <typeInfo type=\"UInt32\" isInnate=\"true\" isViewable=\"true\" groupingRange=\"Enumerated\"/>\r\n"
               L"      <labelInfo label=\"ピン止め\"/>\r\n"
               L"      <displayInfo displayType=\"Enumerated\" defaultColumnWidth=\"12\">\r\n"
               L"        <enumeratedList><enum name=\"Pinned\" value=\"0\" text=\"ピン止め\"/></enumeratedList>\r\n"
               L"      </displayInfo>\r\n"
               L"    </propertyDescription>\r\n";
    }
    xml += L"  </propertyDescriptionList>\r\n</schema>\r\n";
    SHCreateDirectoryExW(nullptr, ParentPath(path).c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    const WCHAR bom = 0xFEFF;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    WriteFile(h, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

void WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, data, (DWORD)size, &w, nullptr);
    CloseHandle(h);
}

// A small 24-bit bitmap, so that Explorer has thumbnails to extract.
void WriteBitmap(const std::wstring& path, int seed) {
    const int width = 48, height = 32, stride = width * 3;
    std::vector<BYTE> file(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + stride * height);
    auto* fh = reinterpret_cast<BITMAPFILEHEADER*>(file.data());
    auto* ih = reinterpret_cast<BITMAPINFOHEADER*>(file.data() + sizeof(BITMAPFILEHEADER));
    fh->bfType = 0x4D42;
    fh->bfSize = (DWORD)file.size();
    fh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    ih->biSize = sizeof(BITMAPINFOHEADER);
    ih->biWidth = width;
    ih->biHeight = height;
    ih->biPlanes = 1;
    ih->biBitCount = 24;
    BYTE* px = file.data() + fh->bfOffBits;
    for (int i = 0; i < stride * height; i++) px[i] = (BYTE)(seed * 31 + i * 7);
    WriteBytes(path, file.data(), file.size());
}

// A folder like a well-used Downloads folder: about 700 items of many kinds.
void PopulateLargeFolder(const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    static const BYTE kEmptyZip[22] = {'P', 'K', 5, 6};
    const char pdf[] = "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj 2 0 obj<</Type/Pages/Kids[]/Count 0>>endobj\n"
                       "trailer<</Root 1 0 R>>\n%%EOF\n";
    wchar_t name[64];
    for (int i = 0; i < 40; i++) {
        swprintf_s(name, L"\\folder_%03d", i);
        CreateDirectoryW((dir + name).c_str(), nullptr);
        if (i % 2 == 0) WriteBitmap(dir + name + L"\\inside.bmp", i);
    }
    for (int i = 0; i < 100; i++) {
        swprintf_s(name, L"\\setup_%03d.exe", i);
        CopyFileW(g_exe.c_str(), (dir + name).c_str(), FALSE);
    }
    for (int i = 0; i < 200; i++) {
        swprintf_s(name, L"\\photo_%03d.bmp", i);
        WriteBitmap(dir + name, i);
    }
    for (int i = 0; i < 60; i++) {
        swprintf_s(name, L"\\archive_%03d.zip", i);
        WriteBytes(dir + name, kEmptyZip, sizeof(kEmptyZip));
    }
    for (int i = 0; i < 60; i++) {
        swprintf_s(name, L"\\doc_%03d.pdf", i);
        WriteBytes(dir + name, pdf, sizeof(pdf) - 1);
    }
    for (int i = 0; i < 240; i++) {
        swprintf_s(name, L"\\note_%03d.txt", i);
        TouchFile(dir + name, 2000 + i % 26);
    }
}

// Number of lines in the agent's log that contain `text`.
int CountLogLines(const wchar_t* text) {
    std::wstring path = LogDirectory() + L"\\agent.log";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::string data;
    char buf[65536];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) data.append(buf, n);
    CloseHandle(h);
    char needle[256];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, needle, sizeof(needle), nullptr, nullptr);
    int count = 0;
    for (size_t pos = data.find(needle); pos != std::string::npos; pos = data.find(needle, pos + 1)) count++;
    return count;
}

// Number of times the agent has changed a view's grouping to the pinned grouping.
int Groupings() { return CountLogLines(L"] group ") + CountLogLines(L"] regroup "); }

// ---------------------------------------------------------------- file dialogs
// "ExplorerPinnedE2E host <folder>" shows an Open dialog on <folder> (the 32-bit build of
// this program shows a dialog of a 32-bit program).
int HostDialog(const std::wstring& folder) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    IShellItem* item = nullptr;
    CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (dialog && item) {
        dialog->SetFolder(item);
        dialog->Show(nullptr);
    }
    if (item) item->Release();
    if (dialog) dialog->Release();
    CoUninitialize();
    return 0;
}

std::wstring ImageName(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return result;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) result = FileNamePart(path);
    CloseHandle(h);
    return result;
}

bool HasShellView(HWND hwnd) {
    bool found = false;
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM lp) -> BOOL {
            wchar_t cls[64];
            if (GetClassNameW(child, cls, 64) && wcscmp(cls, L"SHELLDLL_DefView") == 0) {
                *reinterpret_cast<bool*>(lp) = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// A visible top-level window of a process that `match` accepts: a file dialog, or (with a
// title) a window whose title contains it.
HWND WaitWindow(const std::function<bool(DWORD)>& match, const wchar_t* title, DWORD timeoutMs) {
    struct Search {
        const std::function<bool(DWORD)>* match;
        const wchar_t* title;
        HWND found;
    } search = {&match, title, nullptr};
    WaitFor(
        [&] {
            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL {
                    auto* s = reinterpret_cast<Search*>(lp);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (!IsWindowVisible(hwnd) || !(*s->match)(pid)) return TRUE;
                    if (s->title) {
                        wchar_t text[256] = L"";
                        GetWindowTextW(hwnd, text, 256);
                        if (!wcsstr(text, s->title)) return TRUE;
                    } else {
                        wchar_t cls[32];
                        if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"#32770") != 0 || !HasShellView(hwnd)) return TRUE;
                    }
                    s->found = hwnd;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&search));
            return search.found != nullptr;
        },
        timeoutMs);
    return search.found;
}

HWND WaitFileDialogOf(DWORD pid, DWORD timeoutMs = 20000) {
    return WaitWindow([pid](DWORD p) { return p == pid; }, nullptr, timeoutMs);
}

// Shows a folder in a dialog by typing its path into the file name box.
void NavigateDialog(HWND dialog, const std::wstring& folder) {
    HWND combo = GetDlgItem(dialog, 1148);
    HWND edit = combo ? FindWindowExW(combo, nullptr, L"ComboBox", nullptr) : nullptr;
    edit = edit ? FindWindowExW(edit, nullptr, L"Edit", nullptr) : (combo ? FindWindowExW(combo, nullptr, L"Edit", nullptr) : nullptr);
    if (!edit) {
        Print(L"no file name box in the dialog");
        return;
    }
    SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str()));
    PostMessageW(dialog, WM_COMMAND, IDOK, 0);
}

// Clicks the element named `name` through UI Automation, on a worker thread (a browser may
// return from the click only once the dialog it opened closes).
struct ClickRequest {
    HWND hwnd;
    std::wstring name;
};
DWORD WINAPI ClickThread(LPVOID param) {
    std::unique_ptr<ClickRequest> req(static_cast<ClickRequest*>(param));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    IUIAutomationElement* root = nullptr;
    if (uia) uia->ElementFromHandle(req->hwnd, &root);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(req->name.c_str());
    IUIAutomationCondition* cond = nullptr;
    if (uia) uia->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
    IUIAutomationElement* el = nullptr;
    for (int i = 0; i < 30 && root && cond && !el; i++) {
        root->FindFirst(TreeScope_Descendants, cond, &el);
        if (!el) Sleep(500);
    }
    if (el) {
        IUIAutomationInvokePattern* invoke = nullptr;
        el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
        if (invoke) {
            invoke->Invoke();
            invoke->Release();
        }
        el->Release();
    } else {
        Print(L"\"" + req->name + L"\" not found");
    }
    VariantClear(&v);
    if (cond) cond->Release();
    if (root) root->Release();
    if (uia) uia->Release();
    CoUninitialize();
    return 0;
}
HANDLE ClickAsync(HWND hwnd, const std::wstring& name) {
    return CreateThread(nullptr, 0, ClickThread, new ClickRequest{hwnd, name}, 0, nullptr);
}

PROCESS_INFORMATION StartProcess(const std::wstring& cmdLine, bool dialogSupport = true) {
    std::wstring c = cmdLine;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!dialogSupport) SetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", L"1");
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) Print(L"cannot start " + cmdLine);
    SetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", nullptr);
    return pi;
}

void CloseDialog(HWND dialog, PROCESS_INFORMATION& pi) {
    PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    if (pi.hProcess) {
        DWORD start = GetTickCount();
        while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && GetTickCount() - start < 10000) Pump(50);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    pi = {};
    Pump(500);
}

bool ExpectDialogPinned(HWND dialog, const std::set<std::wstring>& expected, const std::wstring& what) {
    Groups last;
    bool ok = WaitFor(
        [&] {
            last = ReadGroups(dialog);
            return !last.order.empty() && IsPinnedLabel(last.order[0]) && last.First() == expected;
        },
        15000);
    std::vector<std::wstring> exp(expected.begin(), expected.end());
    Check(ok, what, L"expected pinned {" + Join(exp) + L"}, got " + last.Describe());
    return ok;
}

// What the folder shows in a dialog without dialog support: the grouping saved for it.
void ExpectSavedGroupingKept(const std::wstring& host, const std::wstring& folder, const std::wstring& what) {
    PROCESS_INFORMATION pi = StartProcess(L"\"" + host + L"\" host \"" + folder + L"\"", /*dialogSupport=*/false);
    HWND dialog = WaitFileDialogOf(pi.dwProcessId);
    Groups last;
    bool ok = dialog && WaitFor(
                            [&] {
                                last = ReadGroups(dialog);
                                return !last.order.empty() && last.order == std::vector<std::wstring>{L"(none)"};
                            },
                            8000);
    Check(ok, what, last.Describe());
    if (dialog) CloseDialog(dialog, pi);
}

std::wstring ProgramFilesDir() {
    PWSTR p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p))) result = p;
    CoTaskMemFree(p);
    return result;
}

bool MachineKeyExists(const std::wstring& key, REGSAM view) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

const wchar_t kOverlayKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\    ExplorerPinned";
const wchar_t kTestBag[] =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\99999\\ComDlg\\{7D49D726-3C21-4F05-99AA-FDC2C9474656}";

// The upload dialog of a web browser, opened from a page with a file button.
void BrowserUploadTest(const wchar_t* name, const wchar_t* exeName, const std::vector<std::wstring>& candidates,
                       const std::wstring& folder, const std::set<std::wstring>& expected) {
    std::wstring exe;
    for (const auto& c : candidates)
        if (PathExists(c)) exe = c;
    if (exe.empty()) {
        Print(std::wstring(name) + L" is not installed; skipped");
        return;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring page = std::wstring(tmp) + L"ep_upload.html";
    {
        HANDLE h = CreateFileW(page.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const char html[] = "<!doctype html><title>ep-upload</title><input type=file id=f style=display:none>"
                            "<button onclick=\"document.getElementById('f').click()\">pickfile</button>";
        DWORD w;
        WriteFile(h, html, sizeof(html) - 1, &w, nullptr);
        CloseHandle(h);
    }
    std::wstring url = L"file:///" + page;
    std::replace(url.begin(), url.end(), L'\\', L'/');
    std::wstring profile = std::wstring(tmp) + L"ep_profile_" + name;
    std::wstring cmd = L"\"" + exe + L"\" --user-data-dir=\"" + profile +
                       L"\" --no-first-run --no-default-browser-check --disable-search-engine-choice-screen "
                       L"--force-renderer-accessibility \"" + url + L"\"";
    // The browser starts more processes; a job ends them all.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring what = std::wstring(name) + L" upload dialog: pinned group on top";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Check(false, what, L"cannot start " + exe);
        CloseHandle(job);
        return;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    auto isBrowser = [exeName](DWORD pid) { return _wcsicmp(ImageName(pid).c_str(), exeName) == 0; };
    HWND window = WaitWindow(isBrowser, L"ep-upload", 30000);
    HWND dialog = nullptr;
    if (window) {
        Pump(2000);
        CloseHandle(ClickAsync(window, L"pickfile"));
        dialog = WaitWindow(isBrowser, nullptr, 25000);
    }
    if (dialog) {
        Pump(1500);
        NavigateDialog(dialog, folder);
        ExpectDialogPinned(dialog, expected, what);
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        Pump(1500);
    } else {
        Check(false, what, window ? L"no dialog" : L"no browser window");
    }
    TerminateJobObject(job, 0);
    CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Pump(1000);
}

void FileDialogTests(const std::wstring& binDir) {
    // The DLL is loaded by every program that shows a file dialog: it is only registered
    // from Program Files, where only administrators can change it.
    Check(RunExe(L"register-dialogs") != 0, L"dialog support is not registered from a user-writable folder");
    std::wstring installed = ProgramFilesDir() + L"\\Explorer Pinned E2E";
    SHCreateDirectoryExW(nullptr, installed.c_str(), nullptr);
    CopyFileW(g_exe.c_str(), (installed + L"\\ExplorerPinned.exe").c_str(), FALSE);
    CopyFileW((binDir + L"\\ExplorerPinnedShell.dll").c_str(), (installed + L"\\ExplorerPinnedShell.dll").c_str(), FALSE);
    CopyFileW((binDir + L"\\x86\\ExplorerPinnedShell32.dll").c_str(), (installed + L"\\ExplorerPinnedShell32.dll").c_str(), FALSE);
    {
        std::wstring saved = g_exe;
        g_exe = installed + L"\\ExplorerPinned.exe";
        Check(RunExe(L"register-dialogs") == 0, L"register-dialogs exits with 0");
        g_exe = saved;
    }
    std::wstring status = RunExeOutput(L"status");
    Check(status.find(L"dialogs: registered (on)") != std::wstring::npos && status.find(L"ExplorerPinnedShell32.dll") != std::wstring::npos,
          L"dialog support registered for 64-bit and 32-bit programs", status);

    std::wstring folder = g_dir + L"\\dialog";
    SHCreateDirectoryExW(nullptr, (folder + L"\\subfolder_ep").c_str(), nullptr);
    TouchFile(folder + L"\\d1.txt", 2020);
    TouchFile(folder + L"\\d2.txt", 2021);
    TouchFile(folder + L"\\d3.txt", 2022);
    TouchFile(folder + L"\\subfolder_ep\\s1.txt", 2022);
    RunExe(L"pin \"" + folder + L"\\d2.txt\" \"" + folder + L"\\subfolder_ep\"");
    std::wstring self = binDir + L"\\ExplorerPinnedE2E.exe";
    std::wstring hostCmd = L"\"" + self + L"\" host \"" + folder + L"\"";

    // Pinned group, and changes to the pins while the dialog is open
    PROCESS_INFORMATION pi = StartProcess(hostCmd);
    HWND dialog = WaitFileDialogOf(pi.dwProcessId);
    Check(dialog != nullptr, L"file dialog opens");
    if (dialog) {
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog: pinned group on top");
        RunExe(L"unpin \"" + folder + L"\\d2.txt\"");
        ExpectDialogPinned(dialog, {L"subfolder_ep"}, L"file dialog: unpinning updates the open dialog");
        RunExe(L"pin \"" + folder + L"\\d2.txt\"");
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog: pinning updates the open dialog");
        CloseDialog(dialog, pi);
    }
    // Dialogs save the grouping of a folder when they leave it: it must be the folder's own.
    ExpectSavedGroupingKept(self, folder, L"file dialog: closing leaves the folder's grouping as it was");

    pi = StartProcess(hostCmd);
    dialog = WaitFileDialogOf(pi.dwProcessId);
    if (dialog) {
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog: pinned group before opening a subfolder");
        HANDLE click = ClickAsync(dialog, L"subfolder_ep");
        WaitForSingleObject(click, 20000);
        CloseHandle(click);
        Pump(2000);
        CloseDialog(dialog, pi);
    }
    ExpectSavedGroupingKept(self, folder, L"file dialog: opening a subfolder leaves the folder's grouping as it was");

    // Turned off and on in the notification area menu (the setting is read while dialogs are open)
    pi = StartProcess(hostCmd);
    dialog = WaitFileDialogOf(pi.dwProcessId);
    if (dialog) {
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog: pinned group before turning it off");
        RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Dialogs", L"0");
        Groups last;
        Check(WaitFor(
                  [&] {
                      last = ReadGroups(dialog);
                      return last.order == std::vector<std::wstring>{L"(none)"};
                  },
                  10000),
              L"file dialog: turning it off restores the open dialog", last.Describe());
        Check(RunExeOutput(L"status").find(L"dialogs: registered (off)") != std::wstring::npos, L"status shows dialog support off");
        RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Dialogs", L"1");
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog: turning it on again");
        CloseDialog(dialog, pi);
    }

    // A 32-bit program
    std::wstring host32 = binDir + L"\\x86\\ExplorerPinnedE2E.exe";
    pi = StartProcess(L"\"" + host32 + L"\" host \"" + folder + L"\"");
    dialog = pi.hProcess ? WaitFileDialogOf(pi.dwProcessId) : nullptr;
    Check(dialog != nullptr, L"file dialog of a 32-bit program opens", host32);
    if (dialog) {
        ExpectDialogPinned(dialog, {L"d2", L"subfolder_ep"}, L"file dialog of a 32-bit program: pinned group on top");
        CloseDialog(dialog, pi);
    }

    // Web browsers (choosing a file to upload)
    wchar_t pf[MAX_PATH], pf86[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%", pf86, MAX_PATH);
    BrowserUploadTest(L"Edge", L"msedge.exe",
                      {std::wstring(pf86) + L"\\Microsoft\\Edge\\Application\\msedge.exe",
                       std::wstring(pf) + L"\\Microsoft\\Edge\\Application\\msedge.exe"},
                      folder, {L"d2", L"subfolder_ep"});
    BrowserUploadTest(L"Chrome", L"chrome.exe",
                      {std::wstring(pf86) + L"\\Google\\Chrome\\Application\\chrome.exe",
                       std::wstring(pf) + L"\\Google\\Chrome\\Application\\chrome.exe"},
                      folder, {L"d2", L"subfolder_ep"});
    ExpectSavedGroupingKept(self, folder, L"browser upload dialogs leave the folder's grouping as it was");
    RunExe(L"unpin \"" + folder + L"\\d2.txt\" \"" + folder + L"\\subfolder_ep\"");

    // A grouping that a program left behind (it ended while showing a pinned folder); uninstall resets it.
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTestBag, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"GroupByKey:FMTID", 0, REG_SZ, reinterpret_cast<const BYTE*>(kPinStateFormatId),
                       sizeof(kPinStateFormatId));
        DWORD pid = 2;
        RegSetValueExW(k, L"GroupByKey:PID", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&pid), sizeof(pid));
        RegCloseKey(k);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && wcscmp(argv[1], L"host") == 0) return HostDialog(argv[2]);
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

    // --- a machine where version 1.0.x registered ExplorerPinned.propdesc from two folders
    // and one of the files was deleted afterwards (registration then fails partly and
    // Explorer can show every item as "Unspecified"); setup must clean that up
    {
        std::wstring first = std::wstring(tmp) + L"ep_old_portable\\ExplorerPinned.propdesc";
        std::wstring second = std::wstring(tmp) + L"ep_old_installed\\ExplorerPinned.propdesc";
        WriteLegacySchema(first);
        WriteLegacySchema(second);
        HRESULT a = PSRegisterPropertySchema(first.c_str());
        HRESULT b = PSRegisterPropertySchema(second.c_str());
        DeleteFileW(first.c_str());
        RemoveDirectoryW(ParentPath(first).c_str());
        PSRefreshPropertySchema();
        wchar_t text[96];
        swprintf_s(text, L"old registrations: 0x%08lx, 0x%08lx", (unsigned long)a, (unsigned long)b);
        Print(text);
        Print(L"schemas before setup: " + RegisteredSchemas());
        // On a real machine the old registration is days old. Explorer applies schema changes
        // in the background; changing them again within seconds can leave it showing every
        // item as "Unspecified" until a new window is opened, so let it settle first.
        Pump(15000);
    }

    // --- setup (in Japanese, like the user's machine)
    RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Language", L"ja");
    Check(RunExe(L"setup --no-startup --no-agent --quiet") == 0, L"setup exits with 0");
    {
        std::wstring schemas = RegisteredSchemas();
        Print(L"schemas after setup: " + schemas);
        Check(schemas.find(L"\\ExplorerPinned.propdesc") == std::wstring::npos,
              L"setup removes the registrations of version 1.0.x", schemas);
        Check(schemas.find(L"\\ExplorerPinned\\ExplorerPinnedGroup.propdesc") != std::wstring::npos &&
                  schemas.find(L"ExplorerPinnedGroup.propdesc") == schemas.rfind(L"ExplorerPinnedGroup.propdesc"),
              L"setup registers one schema in ProgramData", schemas);
        Check(CountLogLines(L"RegisterSchema(") >= 1 && CountLogLines(L"hr=0x000401a0") == 0,
              L"schema registration succeeds completely");
    }
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
        Check(!g.First().count(L"echo") && !g.order.empty() && IsPinnedLabel(g.order[0]),
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

    // --- a folder Explorer saved with the grouping of version 1.0.x (old property key)
    {
        std::wstring old = g_dir + L"\\oldgroup";
        CreateDirectoryW(old.c_str(), nullptr);
        TouchFile(old + L"\\g1.txt", 2020);
        TouchFile(old + L"\\g2.txt", 2021);
        RunExe(L"pin \"" + old + L"\\g1.txt\"");
        w.Navigate(old);
        ExpectPinned(w, {L"g1"}, L"old grouping: folder is grouped");
        w.Refresh();
        w.view->SetGroupBy(kLegacyPinStateKeys[0], TRUE);
        Pump(1500);
        w.Navigate(L"C:\\Windows");
        w.Navigate(old);
        ExpectPinned(w, {L"g1"}, L"old grouping: replaced by the pinned group when the folder opens");
        RunExe(L"unpin \"" + old + L"\\g1.txt\"");
        ExpectNoPinnedGroup(w, L"old grouping: grouping is removed after unpinning");
        PROPERTYKEY k = w.GroupBy();
        Check(!IsEqualPropertyKey(k, kLegacyPinStateKeys[0]) && !IsEqualPropertyKey(k, kLegacyPinStateKeys[1]),
              L"old grouping: not restored to the old pinned grouping", L"group-by pid " + std::to_wstring(k.pid));
        w.Navigate(g_dir);
    }

    // --- a large folder (like a well-used Downloads folder) in large-icon view
    {
        std::wstring large = g_dir + L"\\large";
        PopulateLargeFolder(large);
        RunExe(L"pin \"" + large + L"\\photo_050.bmp\" \"" + large + L"\\folder_007\"");
        w.Navigate(large);
        w.Refresh();
        w.view->SetViewModeAndIconSize(FVM_ICON, 96);
        WaitForItems(w, L"large folder");
        ExpectPinned(w, {L"photo_050", L"folder_007"}, L"large folder: pinned group on top in large-icon view");

        // Signing in again: Explorer opens the folder with the pinned grouping it saved (the
        // agent was not running to put the original grouping back) and the agent starts later.
        TerminateProcess(agent.hProcess, 0);
        WaitForSingleObject(agent.hProcess, 10000);
        CloseHandle(agent.hProcess);
        CloseHandle(agent.hThread);
        agent = {};
        w.Navigate(L"C:\\Windows");
        w.Navigate(large);
        Check(KeyIndexOf(w.GroupBy()) >= 0, L"large folder: Explorer reopens it with the saved pinned grouping");
        int before = Groupings();
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &agent);
        Check(WaitFor([] { return FindWindowW(kAgentWindowClass, nullptr) != nullptr; }, 10000), L"agent starts again");
        ExpectPinned(w, {L"photo_050", L"folder_007"}, L"large folder: pinned group when the agent starts later");
        Pump(10000);
        int groupings = Groupings() - before;
        Check(groupings >= 1 && groupings <= 2, L"large folder: the agent does not regroup over and over",
              std::to_wstring(groupings) + L" grouping(s)");
        ExpectPinned(w, {L"photo_050", L"folder_007"}, L"large folder: pinned group stays");
        RunExe(L"unpin \"" + large + L"\\photo_050.bmp\" \"" + large + L"\\folder_007\"");
        ExpectNoPinnedGroup(w, L"large folder: grouping is removed after unpinning");
        w.Navigate(g_dir);

        // The same folder through a network path: Explorer loads it in the background and
        // reloads it for a while after every grouping change. (Version 1.0.0 regrouped this
        // folder every second, showing "Working on it..." and only the "Unspecified" group.)
        std::wstring unc = L"\\\\localhost\\" + large.substr(0, 1) + L"$" + large.substr(2);
        if (!PathExists(unc)) {
            Print(L"skipped the network folder checks: " + unc + L" is not reachable");
        } else {
            RunExe(L"pin \"" + unc + L"\\photo_050.bmp\" \"" + unc + L"\\folder_007\"");
            before = Groupings();
            w.Navigate(unc);
            w.Refresh();
            w.view->SetViewModeAndIconSize(FVM_ICON, 96);
            WaitForItems(w, L"network folder");
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group on top");
            Pump(8000);
            groupings = Groupings() - before;
            Check(groupings >= 1 && groupings <= 3, L"network folder: the agent does not regroup over and over",
                  std::to_wstring(groupings) + L" grouping(s)");
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group stays");

            // Opened again with the pinned grouping Explorer saved when the window left it.
            w.Navigate(L"C:\\Windows");
            before = Groupings();
            w.Navigate(unc);
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group when opened again");
            Pump(8000);
            groupings = Groupings() - before;
            Check(groupings >= 1 && groupings <= 3, L"network folder opened again: no regroup loop",
                  std::to_wstring(groupings) + L" grouping(s)");
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group stays after opening again");

            // Signing in again: the agent starts while Explorer is still loading the folder.
            TerminateProcess(agent.hProcess, 0);
            WaitForSingleObject(agent.hProcess, 10000);
            CloseHandle(agent.hProcess);
            CloseHandle(agent.hThread);
            agent = {};
            w.Navigate(L"C:\\Windows");
            before = Groupings();
            {
                PIDLIST_ABSOLUTE pidl = nullptr;
                SHParseDisplayName(unc.c_str(), nullptr, &pidl, 0, nullptr);
                IServiceProvider* sp = nullptr;
                IShellBrowser* sb = nullptr;
                w.browser->QueryInterface(IID_PPV_ARGS(&sp));
                if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb));
                if (sb) sb->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
                if (sb) sb->Release();
                if (sp) sp->Release();
                CoTaskMemFree(pidl);
            }
            CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &agent);
            WaitFor([&] { return w.Refresh() && _wcsicmp(w.Path().c_str(), unc.c_str()) == 0; }, 10000);
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group when the agent starts during the load");
            Pump(8000);
            groupings = Groupings() - before;
            Check(groupings >= 1 && groupings <= 3, L"network folder at sign-in: no regroup loop",
                  std::to_wstring(groupings) + L" grouping(s)");
            ExpectPinned(w, {L"photo_050", L"folder_007"}, L"network folder: pinned group stays after sign-in");

            RunExe(L"unpin \"" + unc + L"\\photo_050.bmp\" \"" + unc + L"\\folder_007\"");
            ExpectNoPinnedGroup(w, L"network folder: grouping is removed after unpinning");
            w.Navigate(g_dir);
        }
        Check(CountLogLines(L"giving up") == 0, L"large folders: the agent never gave up");
    }

    // --- file dialogs (Open/Save, uploads in web browsers)
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        FileDialogTests(ParentPath(self));
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

    // --- closing a window puts the original grouping back before Explorer saves it
    std::wstring fourth = g_dir + L"\\fourth";
    CreateDirectoryW(fourth.c_str(), nullptr);
    TouchFile(fourth + L"\\f1.txt", 2020);
    TouchFile(fourth + L"\\f2.txt", 2021);
    RunExe(L"pin \"" + fourth + L"\\f1.txt\"");
    {
        Window w2;
        if (OpenWindow(&w2, fourth)) {
            ExpectPinned(w2, {L"f1"}, L"second window is grouped");
            w2.browser->Quit();
            Pump(1500);
        } else {
            Check(false, L"open a second window");
        }
    }

    // --- exit restores the grouping; uninstall cleans up
    Check(RunExe(L"exit") == 0, L"exit command exits with 0");
    Check(WaitFor([] { return FindWindowW(kAgentWindowClass, nullptr) == nullptr; }, 10000), L"exit stops the agent");
    ExpectNoPinnedGroup(w, L"exit restores the original grouping");
    w.Navigate(fourth);
    ExpectNoPinnedGroup(w, L"a closed window's folder keeps its original grouping");
    w.Navigate(other);
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        IPersistFolder2* pf = nullptr;
        if (w.Refresh() && SUCCEEDED(w.view->GetFolder(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->GetCurFolder(&pidl))) {
            PWSTR name = nullptr;
            SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEEDITING, &name);
            Print(L"test window shows " + std::wstring(name ? name : L"?") + L" size=" + std::to_wstring(ILGetSize(pidl)));
            CoTaskMemFree(name);
            CoTaskMemFree(pidl);
        }
        if (pf) pf->Release();
    }
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
    {
        IPersistFolder2* pf = nullptr;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (w.Refresh() && SUCCEEDED(w.view->GetFolder(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->GetCurFolder(&pidl))) {
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegGroupedFolders, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k,
                                nullptr) == ERROR_SUCCESS) {
                std::wstring name = third + L"|0";
                RegSetValueExW(k, name.c_str(), 0, REG_BINARY, reinterpret_cast<const BYTE*>(pidl), ILGetSize(pidl));
                RegCloseKey(k);
            }
            CoTaskMemFree(pidl);
        }
        if (pf) pf->Release();
    }
    w.Navigate(g_dir);
    // uninstall deletes the agent's log; keep a copy for the test output
    CopyFileW((LogDirectory() + L"\\agent.log").c_str(), (std::wstring(tmp) + L"ep_agent.log").c_str(), FALSE);
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
        Check(!PathExists(LogDirectory()), L"uninstall removes the log", LogDirectory());
        wchar_t programData[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
        Check(!PathExists(std::wstring(programData) + L"\\ExplorerPinned"), L"uninstall removes the schema file");
        auto file = MenuItems(g_dir + L"\\alpha.txt");
        Check(!Contains(file, pinLabel), L"uninstall removes the menu", Join(file));
        Check(!MachineKeyExists(kOverlayKey, KEY_WOW64_64KEY) && !MachineKeyExists(kOverlayKey, KEY_WOW64_32KEY),
              L"uninstall unregisters the dialog support");
        std::wstring fmtid;
        RegReadString(HKEY_CURRENT_USER, kTestBag, L"GroupByKey:FMTID", &fmtid);
        Check(fmtid == L"{00000000-0000-0000-0000-000000000000}", L"uninstall resets groupings that dialogs saved", fmtid);
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\99999");
        // Best effort: programs that showed a dialog may still have the DLL loaded.
        std::wstring installed = ProgramFilesDir() + L"\\Explorer Pinned E2E";
        for (const wchar_t* f : {L"\\ExplorerPinned.exe", L"\\ExplorerPinnedShell.dll", L"\\ExplorerPinnedShell32.dll"})
            DeleteFileW((installed + f).c_str());
        RemoveDirectoryW(installed.c_str());
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
