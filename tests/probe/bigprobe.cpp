// Research probe: grouping by a view property in large folders (icon view, mixed file types, network, logon).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <exdisp.h>
#include <exdispid.h>
#include <ocidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <uiautomation.h>
#include <wincodec.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>

#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif

static const GUID FMTID_EP = {0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}};
static const PROPERTYKEY KA = {FMTID_EP, 2};
static const PROPERTYKEY KB = {FMTID_EP, 3};
static const PROPERTYKEY KNONE = {GUID_NULL, 0};
static const GUID kFmtStorage = {0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
static const PROPERTYKEY K_DateModified = {kFmtStorage, 14};
static const PROPERTYKEY K_ItemTypeText = {kFmtStorage, 4};

static DWORD g_t0;
static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[8192];
    int p = swprintf_s(buf, L"%6lu ", GetTickCount() - g_t0);
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf + p, _countof(buf) - p, _TRUNCATE, fmt, ap);
    va_end(ap);
    char out[16384];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL);
    if (n > 0) {
        fputs(out, stdout);
        fputs("\n", stdout);
        fflush(stdout);
    }
}

static std::wstring KeyName(const PROPERTYKEY& k) {
    if (IsEqualPropertyKey(k, KA)) return L"A";
    if (IsEqualPropertyKey(k, KB)) return L"B";
    if (IsEqualPropertyKey(k, KNONE)) return L"none";
    if (IsEqualPropertyKey(k, K_DateModified)) return L"DateModified";
    if (IsEqualPropertyKey(k, K_ItemTypeText)) return L"ItemType";
    wchar_t b[64];
    swprintf_s(b, L"pid%lu", k.pid);
    return b;
}

// ---------------- events ----------------
struct EventRec {
    DWORD t;
    wchar_t src;
    DISPID id;
};
static std::vector<EventRec> g_events;
static int g_docComplete = 0;

static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int left = (int)(end - GetTickCount());
        if (left <= 0) break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, std::min(left, 20), QS_ALLINPUT);
    }
}

class Sink : public IDispatch {
    LONG ref_ = 1;
    IID iid_;
    wchar_t src_;

public:
    Sink(REFIID iid, wchar_t src) : iid_(iid), src_(src) {}
    virtual ~Sink() {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == iid_) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP GetTypeInfoCount(UINT* p) override {
        *p = 0;
        return S_OK;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (src_ == L'B' && (id == 102 || id == 105 || id == 112 || id == 113 || id == 106 || id == 104)) return S_OK;
        if (src_ == L'V' && id == 200) return S_OK;  // selection changed
        if (id == 259) g_docComplete++;
        g_events.push_back({GetTickCount() - g_t0, src_, id});
        return S_OK;
    }
};

static void Advise(IUnknown* src, REFIID iid, wchar_t tag) {
    IConnectionPointContainer* cpc = NULL;
    if (FAILED(src->QueryInterface(IID_PPV_ARGS(&cpc)))) return;
    IConnectionPoint* cp = NULL;
    cpc->FindConnectionPoint(iid, &cp);
    cpc->Release();
    if (!cp) return;
    Sink* sink = new Sink(iid, tag);
    DWORD cookie = 0;
    cp->Advise(sink, &cookie);
    sink->Release();
    cp->Release();
}

static std::wstring TakeEvents() {
    std::wstring s;
    for (auto& e : g_events) {
        wchar_t b[48];
        swprintf_s(b, L" %c%ld@%lu", e.src, e.id, e.t);
        s += b;
    }
    g_events.clear();
    return s;
}

// ---------------- files ----------------
static std::wstring g_exe, g_epExe, g_big, g_downloads;

static void SetTimes(const std::wstring& path, int hoursAgo) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER u;
    u.LowPart = now.dwLowDateTime;
    u.HighPart = now.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)hoursAgo * 3600ULL * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
}

static void WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, data, (DWORD)size, &w, NULL);
    CloseHandle(h);
}

static IWICImagingFactory* g_wic = NULL;

static void WriteImage(const std::wstring& path, const GUID& format, int seed) {
    const int W = 96, H = 64;
    std::vector<BYTE> px(W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            BYTE* p = &px[(y * W + x) * 4];
            p[0] = (BYTE)(seed * 37 + x * 2);
            p[1] = (BYTE)(seed * 91 + y * 3);
            p[2] = (BYTE)(seed * 13 + x + y);
            p[3] = 255;
        }
    IWICBitmap* bmp = NULL;
    g_wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGRA, W * 4, (UINT)px.size(), px.data(), &bmp);
    IWICStream* stream = NULL;
    g_wic->CreateStream(&stream);
    stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    IWICBitmapEncoder* enc = NULL;
    g_wic->CreateEncoder(format, NULL, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* frame = NULL;
    enc->CreateNewFrame(&frame, NULL);
    frame->Initialize(NULL);
    frame->SetSize(W, H);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&pf);
    frame->WriteSource(bmp, NULL);
    frame->Commit();
    enc->Commit();
    frame->Release();
    enc->Release();
    stream->Release();
    bmp->Release();
}

static std::vector<std::wstring> PopulateFolder(const std::wstring& dir) {
    SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    std::vector<std::wstring> made;
    static const BYTE kZip[22] = {'P', 'K', 5, 6};
    const char* pdf = "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj 2 0 obj<</Type/Pages/Kids[]/Count 0>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n";
    const char* html = "<!doctype html><title>x</title><p>hello</p>";
    int hours = 1;
    auto add = [&](const std::wstring& name) {
        std::wstring p = dir + L"\\" + name;
        SetTimes(p, hours);
        hours += 5;
        made.push_back(name);
    };
    wchar_t n[64];
    for (int i = 0; i < 40; i++) {
        swprintf_s(n, L"folder_%03d", i);
        std::wstring f = dir + L"\\" + n;
        CreateDirectoryW(f.c_str(), NULL);
        if (i % 2 == 0) WriteImage(f + L"\\inside.png", GUID_ContainerFormatPng, i);
        add(n);
    }
    for (int i = 0; i < 100; i++) {
        swprintf_s(n, L"app_%03d.exe", i);
        CopyFileW(g_exe.c_str(), (dir + L"\\" + n).c_str(), FALSE);
        add(n);
    }
    for (int i = 0; i < 100; i++) {
        swprintf_s(n, L"photo_%03d.png", i);
        WriteImage(dir + L"\\" + n, GUID_ContainerFormatPng, i);
        add(n);
    }
    for (int i = 0; i < 100; i++) {
        swprintf_s(n, L"image_%03d.jpg", i);
        WriteImage(dir + L"\\" + n, GUID_ContainerFormatJpeg, i + 300);
        add(n);
    }
    for (int i = 0; i < 60; i++) {
        swprintf_s(n, L"archive_%03d.zip", i);
        WriteBytes(dir + L"\\" + n, kZip, sizeof(kZip));
        add(n);
    }
    for (int i = 0; i < 60; i++) {
        swprintf_s(n, L"doc_%03d.pdf", i);
        WriteBytes(dir + L"\\" + n, pdf, strlen(pdf));
        add(n);
    }
    for (int i = 0; i < 40; i++) {
        swprintf_s(n, L"page_%03d.html", i);
        WriteBytes(dir + L"\\" + n, html, strlen(html));
        add(n);
    }
    for (int i = 0; i < 184; i++) {
        swprintf_s(n, L"note_%03d.txt", i);
        WriteBytes(dir + L"\\" + n, "hello", 5);
        add(n);
    }
    return made;
}

static std::wstring LongPath(const std::wstring& p) {
    wchar_t lp[MAX_PATH];
    return GetLongPathNameW(p.c_str(), lp, MAX_PATH) ? lp : p;
}

// ---------------- UIA ----------------
struct UiaReq {
    HWND hwnd;
    std::wstring out;
};
static UiaReq g_uia;

static DWORD WINAPI UiaThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation* uia = NULL;
    CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (uia) {
        IUIAutomationElement* root = NULL;
        uia->ElementFromHandle(g_uia.hwnd, &root);
        if (root) {
            VARIANT v;
            v.vt = VT_I4;
            v.lVal = UIA_ListItemControlTypeId;
            IUIAutomationCondition* cond = NULL;
            uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
            IUIAutomationElementArray* arr = NULL;
            root->FindAll(TreeScope_Descendants, cond, &arr);
            IUIAutomationTreeWalker* walker = NULL;
            uia->get_ControlViewWalker(&walker);
            int len = 0;
            if (arr) arr->get_Length(&len);
            struct G {
                int firstY;
                std::vector<std::wstring> items;
            };
            std::map<std::wstring, G> groups;
            for (int i = 0; i < len; i++) {
                IUIAutomationElement* el = NULL;
                arr->GetElement(i, &el);
                BSTR name = NULL;
                el->get_CurrentName(&name);
                IUIAutomationElement* parent = NULL;
                walker->GetParentElement(el, &parent);
                BSTR pname = NULL;
                if (parent) parent->get_CurrentName(&pname);
                RECT r = {};
                el->get_CurrentBoundingRectangle(&r);
                std::wstring gname = pname ? pname : L"";
                auto it = groups.find(gname);
                if (it == groups.end()) it = groups.emplace(gname, G{(int)r.top, {}}).first;
                it->second.firstY = std::min(it->second.firstY, (int)r.top);
                it->second.items.push_back(name ? name : L"");
                SysFreeString(name);
                SysFreeString(pname);
                if (parent) parent->Release();
                el->Release();
            }
            std::vector<std::pair<int, std::wstring>> order;
            for (auto& g : groups) order.push_back({g.second.firstY, g.first});
            std::sort(order.begin(), order.end());
            wchar_t b[64];
            swprintf_s(b, L"realized=%d", len);
            g_uia.out = b;
            for (auto& o : order) {
                auto& g = groups[o.second];
                g_uia.out += L" | [" + o.second + L"] (" + std::to_wstring(g.items.size()) + L"):";
                for (size_t i = 0; i < g.items.size() && i < 5; i++) g_uia.out += L" " + g.items[i];
            }
            if (arr) arr->Release();
            if (walker) walker->Release();
            if (cond) cond->Release();
            root->Release();
        }
        uia->Release();
    }
    CoUninitialize();
    return 0;
}

static std::wstring UiaGroups(HWND hwnd) {
    g_uia.hwnd = hwnd;
    g_uia.out = L"(uia timeout)";
    HANDLE t = CreateThread(NULL, 0, UiaThread, NULL, 0, NULL);
    DWORD end = GetTickCount() + 30000;
    while (WaitForSingleObject(t, 0) == WAIT_TIMEOUT && (int)(end - GetTickCount()) > 0) Pump(20);
    CloseHandle(t);
    return g_uia.out;
}

// ---------------- explorer ----------------
struct View {
    IWebBrowser2* wb = NULL;
    IFolderView2* fv = NULL;
    IShellFolder* sf = NULL;
    IUnknown* id = NULL;
    HWND hwnd = NULL;
    void Release() {
        if (sf) sf->Release();
        if (fv) fv->Release();
        if (id) id->Release();
        sf = NULL;
        fv = NULL;
        id = NULL;
    }
    // Picks up the active view; events are connected once per view object.
    bool Acquire() {
        IServiceProvider* sp = NULL;
        IShellBrowser* sb = NULL;
        IShellView* sv = NULL;
        IFolderView2* nfv = NULL;
        IUnknown* nid = NULL;
        wb->QueryInterface(IID_PPV_ARGS(&sp));
        if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb));
        if (sb) sb->QueryActiveShellView(&sv);
        if (sv) sv->QueryInterface(IID_PPV_ARGS(&nfv));
        if (nfv) nfv->QueryInterface(IID_PPV_ARGS(&nid));
        if (sv) sv->Release();
        if (sb) sb->Release();
        if (sp) sp->Release();
        if (!nfv) {
            Release();
            return false;
        }
        if (nid == id) {
            nfv->Release();
            nid->Release();
            return fv && sf;
        }
        Release();
        fv = nfv;
        id = nid;
        fv->GetFolder(IID_PPV_ARGS(&sf));
        IDispatch* doc = NULL;
        wb->get_Document(&doc);
        if (doc) {
            Advise(doc, DIID_DShellFolderViewEvents, L'V');
            doc->Release();
        }
        return fv && sf;
    }
    std::wstring Path() {
        wchar_t path[MAX_PATH] = L"";
        IPersistFolder2* pf = NULL;
        if (sf && SUCCEEDED(sf->QueryInterface(IID_PPV_ARGS(&pf)))) {
            PIDLIST_ABSOLUTE p = NULL;
            if (SUCCEEDED(pf->GetCurFolder(&p))) {
                SHGetPathFromIDListW(p, path);
                CoTaskMemFree(p);
            }
            pf->Release();
        }
        return path;
    }
};

static View g_view;

static bool OpenWindow(const std::wstring& dir) {
    ShellExecuteW(NULL, L"open", L"explorer.exe", dir.c_str(), NULL, SW_SHOWNORMAL);
    IShellWindows* sw = NULL;
    CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_PPV_ARGS(&sw));
    if (!sw) return false;
    for (int tries = 0; tries < 50; tries++) {
        Pump(200);
        long count = 0;
        sw->get_Count(&count);
        for (long i = 0; i < count; i++) {
            VARIANT vi;
            vi.vt = VT_I4;
            vi.lVal = i;
            IDispatch* disp = NULL;
            if (FAILED(sw->Item(vi, &disp)) || !disp) continue;
            IWebBrowser2* wb = NULL;
            disp->QueryInterface(IID_PPV_ARGS(&wb));
            disp->Release();
            if (!wb) continue;
            g_view.wb = wb;
            if (g_view.Acquire() && _wcsicmp(g_view.Path().c_str(), dir.c_str()) == 0) {
                SHANDLE_PTR h = 0;
                wb->get_HWND(&h);
                g_view.hwnd = (HWND)h;
                Advise(wb, DIID_DWebBrowserEvents2, L'B');
                sw->Release();
                ShowWindow(g_view.hwnd, SW_MAXIMIZE);
                return true;
            }
            g_view.Release();
            wb->Release();
            g_view.wb = NULL;
        }
    }
    sw->Release();
    return false;
}

static void Navigate(const std::wstring& path, bool wait = true) {
    PIDLIST_ABSOLUTE pidl = NULL;
    SHParseDisplayName(path.c_str(), NULL, &pidl, 0, NULL);
    IServiceProvider* sp = NULL;
    IShellBrowser* sb = NULL;
    g_view.wb->QueryInterface(IID_PPV_ARGS(&sp));
    if (sp) sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb));
    int before = g_docComplete;
    HRESULT hr = sb ? sb->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE) : E_FAIL;
    if (sb) sb->Release();
    if (sp) sp->Release();
    CoTaskMemFree(pidl);
    DWORD t = GetTickCount();
    if (!wait) {
        Log(L"  navigate %s (no wait) hr=0x%08x", path.c_str(), hr);
        return;
    }
    while (g_docComplete == before && GetTickCount() - t < 30000) Pump(20);
    Log(L"  navigate took %lums", GetTickCount() - t);
    Pump(300);
    bool ok = g_view.Acquire();
    Log(L"  navigate %s hr=0x%08x acquired=%d path=%s", path.c_str(), hr, ok, g_view.Path().c_str());
}

static PITEMID_CHILD Child(const std::wstring& name) {
    PIDLIST_RELATIVE pidl = NULL;
    ULONG eaten = 0;
    std::wstring b = name;
    if (FAILED(g_view.sf->ParseDisplayName(NULL, NULL, b.data(), &eaten, &pidl, NULL))) return NULL;
    return (PITEMID_CHILD)pidl;
}

static std::vector<std::wstring> g_pins;

static void SetPins(const PROPERTYKEY& key) {
    PROPVARIANT v;
    InitPropVariantFromUInt32(0, &v);
    std::wstring s;
    for (auto& n : g_pins) {
        PITEMID_CHILD c = Child(n);
        HRESULT hr = c ? g_view.fv->SetViewProperty(c, key, v) : E_INVALIDARG;
        if (FAILED(hr)) s += L" " + n + L":" + std::to_wstring(hr);
        if (c) CoTaskMemFree(c);
    }
    Log(L"  set %s on pins%s", KeyName(key).c_str(), s.empty() ? L" ok" : s.c_str());
}

static std::wstring ReadPins(const PROPERTYKEY& key) {
    std::wstring s;
    for (auto& n : g_pins) {
        PITEMID_CHILD c = Child(n);
        PROPVARIANT v;
        PropVariantInit(&v);
        HRESULT hr = c ? g_view.fv->GetViewProperty(c, key, &v) : E_INVALIDARG;
        wchar_t b[64];
        if (FAILED(hr)) swprintf_s(b, L" F%08lx", (unsigned long)hr);
        else if (v.vt == VT_UI4) swprintf_s(b, L" %lu", v.ulVal);
        else swprintf_s(b, L" vt%d", v.vt);
        s += b;
        PropVariantClear(&v);
        if (c) CoTaskMemFree(c);
    }
    return s;
}

static void SetGroup(const PROPERTYKEY& key, BOOL asc = TRUE) {
    DWORD t = GetTickCount();
    HRESULT hr = g_view.fv->SetGroupBy(key, asc);
    Log(L"  SetGroupBy(%s) hr=0x%08x took %lums", KeyName(key).c_str(), hr, GetTickCount() - t);
}

// Logs changes of item count, grouping, pin values on both keys and events.
static void Watch(DWORD ms, const wchar_t* label, const std::function<void()>& tick = nullptr) {
    DWORD end = GetTickCount() + ms;
    std::wstring last;
    int samples = 0;
    while ((int)(end - GetTickCount()) > 0) {
        if (!g_view.Acquire()) {
            Pump(50);
            continue;
        }
        int count = -1;
        HRESULT hc = g_view.fv->ItemCount(SVGIO_ALLVIEW, &count);
        PROPERTYKEY gk = {};
        BOOL asc = TRUE;
        g_view.fv->GetGroupBy(&gk, &asc);
        std::wstring state = L"count=" + std::to_wstring(count) + (FAILED(hc) ? L"(fail)" : L"") + L" group=" +
                             KeyName(gk) + (asc ? L"" : L"(desc)") + L" A:" + ReadPins(KA) + L" B:" + ReadPins(KB);
        std::wstring ev = TakeEvents();
        if (state != last || !ev.empty()) {
            Log(L"   [%s] %s ev:%s", label, state.c_str(), ev.c_str());
            last = state;
        }
        samples++;
        if (tick) tick();
        Pump(50);
    }
}

static void Uia(const wchar_t* label) { Log(L"   UIA[%s] %s", label, UiaGroups(g_view.hwnd).c_str()); }

static void SetIcons() {
    HRESULT hr = g_view.fv->SetViewModeAndIconSize(FVM_ICON, 96);
    Log(L"  large icons hr=0x%08x", hr);
}

// Opens `dir` from a neutral folder with the given grouping, like opening it fresh.
static void Fresh(const std::wstring& dir, bool icons, const PROPERTYKEY* group) {
    Navigate(L"C:\\Windows\\Help");
    Navigate(dir);
    if (icons) SetIcons();
    else g_view.fv->SetCurrentViewMode(FVM_DETAILS);
    if (group) SetGroup(*group);
    Watch(2500, L"settle");
}

// ---------------- agent ----------------
static void RunEp(const std::wstring& args, bool wait) {
    std::wstring cmd = L"\"" + g_epExe + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Log(L"  CreateProcess failed %lu: %s", GetLastError(), cmd.c_str());
        return;
    }
    if (wait) {
        while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) Pump(50);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static std::wstring g_logPath;
static size_t g_logPos = 0;

static void DumpAgentLog(const wchar_t* label) {
    HANDLE h = CreateFileW(g_logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"   agent.log[%s] missing", label);
        return;
    }
    std::string data;
    char buf[65536];
    DWORD r;
    while (ReadFile(h, buf, sizeof(buf), &r, NULL) && r) data.append(buf, r);
    CloseHandle(h);
    std::string part = data.size() > g_logPos ? data.substr(g_logPos) : std::string();
    g_logPos = data.size();
    int regroups = 0, groups = 0;
    size_t p = 0;
    while ((p = part.find("regroup ", p)) != std::string::npos) {
        regroups++;
        p++;
    }
    p = 0;
    while ((p = part.find("] group ", p)) != std::string::npos) {
        groups++;
        p++;
    }
    Log(L"   agent.log[%s] group=%d regroup=%d", label, groups, regroups);
    if (part.size() > 3000) part = part.substr(0, 1500) + "\n...\n" + part.substr(part.size() - 1500);
    int n = MultiByteToWideChar(CP_UTF8, 0, part.c_str(), (int)part.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, part.c_str(), (int)part.size(), w.data(), n);
    Log(L"%s", w.c_str());
}

// ---------------- trials ----------------
static void AlternateTrial(const wchar_t* label) {
    SetPins(KA);
    SetGroup(KA);
    Watch(6000, label);
    Uia(label);
    std::wstring l2 = std::wstring(label) + L" ->B";
    SetPins(KB);
    SetGroup(KB);
    Watch(6000, l2.c_str());
    Uia(l2.c_str());
    std::wstring l3 = std::wstring(label) + L" ->A";
    SetPins(KA);
    SetGroup(KA);
    Watch(6000, l3.c_str());
    Uia(l3.c_str());
}

// Leaves the folder grouped by A and comes back: Explorer restores the grouping without values.
static void Relogin(const std::wstring& dir) {
    Fresh(dir, true, &KNONE);
    SetPins(KA);
    SetGroup(KA);
    Watch(4000, L"before leave");
    Navigate(L"C:\\Windows\\Help");
    Navigate(dir);
    Watch(4000, L"restored");
    Uia(L"restored");
}

int wmain(int argc, wchar_t** argv) {
    g_t0 = GetTickCount();
    SetConsoleOutputCP(CP_UTF8);
    OleInitialize(NULL);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    g_exe = exe;
    if (argc > 1) {
        wchar_t full[MAX_PATH];
        GetFullPathNameW(argv[1], MAX_PATH, full, NULL);
        g_epExe = full;
    }
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    SHCreateDirectoryExW(NULL, (std::wstring(tmp) + L"ep_big").c_str(), NULL);
    g_big = LongPath(std::wstring(tmp) + L"ep_big");
    PWSTR dl = NULL;
    SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &dl);
    g_downloads = dl;
    CoTaskMemFree(dl);
    PWSTR la = NULL;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &la);
    g_logPath = std::wstring(la) + L"\\ExplorerPinned\\agent.log";
    CoTaskMemFree(la);

    std::wstring huge = LongPath(std::wstring(tmp)) + L"ep_huge";
    std::wstring unc = L"\\\\localhost\\" + g_big.substr(0, 1) + L"$" + g_big.substr(2);
    auto made = PopulateFolder(g_big);
    PopulateFolder(huge);
    for (int i = 0; i < 20000; i++) {
        wchar_t n[64];
        swprintf_s(n, L"\\bulk_%05d.txt", i);
        WriteBytes(huge + n, "x", 1);
    }
    Log(L"populated %zu items in %s, 20000 more in %s; unc=%s exists=%d", made.size(), g_big.c_str(), huge.c_str(),
        unc.c_str(), GetFileAttributesW(unc.c_str()) != INVALID_FILE_ATTRIBUTES);
    g_pins = {L"folder_007", L"app_003.exe", L"photo_050.png", L"doc_010.pdf", L"note_100.txt"};

    RunEp(L"register-schema --lang en", true);

    if (!OpenWindow(g_big)) {
        Log(L"window not found");
        return 1;
    }
    Log(L"window open");
    Pump(2000);

    Log(L"######## H1 huge folder, large icons, none -> A -> B -> A");
    Fresh(huge, true, &KNONE);
    AlternateTrial(L"H1");
    Log(L"######## H2 huge folder relogin, then B");
    Relogin(huge);
    SetPins(KB);
    SetGroup(KB);
    Watch(8000, L"H2 ->B");
    Uia(L"H2 ->B");

    Log(L"######## N1 network path, large icons, none -> A -> B -> A");
    Fresh(unc, true, &KNONE);
    AlternateTrial(L"N1");
    Log(L"######## N2 network path relogin, then B");
    Relogin(unc);
    SetPins(KB);
    SetGroup(KB);
    Watch(8000, L"N2 ->B");
    Uia(L"N2 ->B");

    Log(L"######## C1 a file in the folder keeps changing");
    {
        DWORD lastTouch = 0;
        int n = 0;
        auto touch = [&] {
            if (GetTickCount() - lastTouch < 300) return;
            lastTouch = GetTickCount();
            std::string data(100 + (n++ % 50), 'x');
            WriteBytes(g_big + L"\\note_000.txt", data.data(), data.size());
        };
        Fresh(g_big, true, &KNONE);
        SetPins(KA);
        SetGroup(KA);
        Watch(6000, L"C1 A", touch);
        Uia(L"C1 A");
        SetPins(KB);
        SetGroup(KB);
        Watch(6000, L"C1 ->B", touch);
        Uia(L"C1 ->B");
        Log(L"######## C2 a pinned file keeps changing");
        auto touchPinned = [&] {
            if (GetTickCount() - lastTouch < 300) return;
            lastTouch = GetTickCount();
            std::string data(100 + (n++ % 50), 'x');
            WriteBytes(g_big + L"\\note_100.txt", data.data(), data.size());
        };
        SetPins(KA);
        SetGroup(KA);
        Watch(6000, L"C2 ->A", touchPinned);
        Uia(L"C2 ->A");
    }

    if (!g_epExe.empty()) {
        HKEY k;
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
        RegSetValueExW(k, L"Log", 0, REG_SZ, (const BYTE*)L"1", 4);
        RegCloseKey(k);
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned\\Pins", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
        for (auto& n : g_pins) {
            for (auto& dir : {g_big, huge, unc}) {
                std::wstring p = dir + L"\\" + n;
                RegSetValueExW(k, p.c_str(), 0, REG_SZ, (const BYTE*)L"x", 4);
            }
        }
        RegCloseKey(k);

        Log(L"######## L1 agent on the huge folder, then logon simulation (agent starts while it loads)");
        Fresh(huge, true, &KNONE);
        RunEp(L"agent", false);
        Watch(15000, L"L1 agent");
        Uia(L"L1 agent");
        DumpAgentLog(L"L1");
        system("taskkill /F /IM ExplorerPinned.exe");
        Navigate(L"C:\\Windows\\Help");
        Navigate(huge, false);
        RunEp(L"agent", false);
        Watch(30000, L"L1 logon");
        Uia(L"L1 logon");
        DumpAgentLog(L"L1 logon");

        Log(L"######## L2 agent on the network path, then logon simulation");
        Navigate(unc);
        Watch(15000, L"L2 agent");
        Uia(L"L2 agent");
        DumpAgentLog(L"L2");
        system("taskkill /F /IM ExplorerPinned.exe");
        Navigate(L"C:\\Windows\\Help");
        Navigate(unc, false);
        RunEp(L"agent", false);
        Watch(30000, L"L2 logon");
        Uia(L"L2 logon");
        DumpAgentLog(L"L2 logon");

        Log(L"######## L3 agent with a pinned file that keeps changing");
        Navigate(g_big);
        {
            DWORD lastTouch = 0;
            int n = 0;
            Watch(45000, L"L3", [&] {
                if (GetTickCount() - lastTouch < 300) return;
                lastTouch = GetTickCount();
                std::string data(100 + (n++ % 50), 'x');
                WriteBytes(g_big + L"\\note_100.txt", data.data(), data.size());
            });
        }
        Uia(L"L3");
        DumpAgentLog(L"L3");
        RunEp(L"exit", true);
    }

    g_view.Release();
    g_view.wb->Quit();
    Log(L"done");
    OleUninitialize();
    return 0;
}
