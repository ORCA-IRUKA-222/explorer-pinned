// Research probe: how property schema registration problems show up in Explorer.
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
static std::wstring g_exe, g_epExe, g_big, g_downloads, g_dir;

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
// ---------------- schema scenarios ----------------
static GUID Fmt(BYTE n) {
    GUID g = {0x5c0e0a10, 0x1d2b, 0x4c3d, {0x8e, 0x4f, 0x50, 0x61, 0x72, 0x83, 0x94, n}};
    return g;
}

static std::wstring GuidStr(const GUID& g) {
    wchar_t b[64];
    StringFromGUID2(g, b, 64);
    return b;
}

static std::wstring Xml(const GUID& fmt, const std::wstring& prefix, const std::wstring& label, const std::wstring& text,
                        const wchar_t* type = L"UInt32") {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"utf-16\"?>\r\n"
        L"<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
        L"  <propertyDescriptionList publisher=\"ProbeCo\" product=\"Probe\">\r\n";
    for (int pid = 2; pid <= 3; pid++) {
        xml += L"    <propertyDescription name=\"" + prefix + (pid == 2 ? L".PinState" : L".PinStateAlt") +
               L"\" formatID=\"" + GuidStr(fmt) + L"\" propID=\"" + std::to_wstring(pid) + L"\">\r\n";
        xml += L"      <searchInfo inInvertedIndex=\"false\" isColumn=\"false\"/>\r\n"
               L"      <typeInfo type=\"" + std::wstring(type) + L"\" isInnate=\"true\" isViewable=\"true\" groupingRange=\"Enumerated\"/>\r\n"
               L"      <labelInfo label=\"" + label + L"\"/>\r\n"
               L"      <displayInfo displayType=\"Enumerated\" defaultColumnWidth=\"12\">\r\n"
               L"        <enumeratedList>\r\n"
               L"          <enum name=\"Pinned\" value=\"0\" text=\"" + text + L"\"/>\r\n"
               L"        </enumeratedList>\r\n"
               L"      </displayInfo>\r\n"
               L"    </propertyDescription>\r\n";
    }
    xml += L"  </propertyDescriptionList>\r\n</schema>\r\n";
    return xml;
}

static void WriteUtf16(const std::wstring& path, const std::wstring& text) {
    SHCreateDirectoryExW(NULL, path.substr(0, path.rfind(L'\\')).c_str(), NULL);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WORD bom = 0xFEFF;
    WriteFile(h, &bom, 2, &w, NULL);
    WriteFile(h, text.c_str(), (DWORD)(text.size() * 2), &w, NULL);
    CloseHandle(h);
}

static HRESULT Reg(const std::wstring& path) {
    HRESULT hr = PSRegisterPropertySchema(path.c_str());
    HRESULT r2 = PSRefreshPropertySchema();
    Log(L"  register %s -> 0x%08lx (refresh 0x%08lx)", path.c_str(), (unsigned long)hr, (unsigned long)r2);
    return hr;
}

static HRESULT Unreg(const std::wstring& path) {
    HRESULT hr = PSUnregisterPropertySchema(path.c_str());
    PSRefreshPropertySchema();
    Log(L"  unregister %s -> 0x%08lx", path.c_str(), (unsigned long)hr);
    return hr;
}

static void DumpSchemas(const wchar_t* label) {
    std::wstring result;
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertySchema", 0,
                      KEY_READ, &root) != ERROR_SUCCESS) {
        Log(L"  schemas[%s]: no key", label);
        return;
    }
    wchar_t sub[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t name[256], data[1024];
        for (DWORD j = 0;; j++) {
            DWORD nl = 256, dl = sizeof(data), type = 0;
            if (RegEnumValueW(k, j, name, &nl, NULL, &type, (BYTE*)data, &dl) != ERROR_SUCCESS) break;
            if (type == REG_SZ || type == REG_EXPAND_SZ) result += std::wstring(L" ") + sub + L"\\" + name + L"=" + data + L";";
            else result += std::wstring(L" ") + sub + L"\\" + name + L"(type " + std::to_wstring(type) + L");";
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    Log(L"  schemas[%s]:%s", label, result.c_str());
}

static void Describe(const PROPERTYKEY& key, const wchar_t* label) {
    IPropertyDescription* d = NULL;
    HRESULT hr = PSGetPropertyDescription(key, IID_PPV_ARGS(&d));
    if (FAILED(hr)) {
        Log(L"  describe[%s] pid %lu: 0x%08lx", label, key.pid, (unsigned long)hr);
        return;
    }
    PWSTR canon = NULL, disp = NULL, fmt = NULL;
    d->GetCanonicalName(&canon);
    d->GetDisplayName(&disp);
    VARTYPE vt = 0;
    d->GetPropertyType(&vt);
    PROPDESC_GROUPING_RANGE gr = PDGR_DISCRETE;
    d->GetGroupingRange(&gr);
    PROPVARIANT v;
    InitPropVariantFromUInt32(0, &v);
    d->FormatForDisplay(v, PDFF_DEFAULT, &fmt);
    Log(L"  describe[%s] pid %lu: name=%s label=%s vt=%d grouping=%d value0=%s", label, key.pid, canon ? canon : L"-",
        disp ? disp : L"-", vt, gr, fmt ? fmt : L"-");
    CoTaskMemFree(canon);
    CoTaskMemFree(disp);
    CoTaskMemFree(fmt);
    d->Release();
}

// Gives the pins the value 0 on `key`, groups by it and shows what Explorer made of it.
static void Try(const PROPERTYKEY& key, const wchar_t* label) {
    Describe(key, label);
    SetGroup(KNONE);
    Pump(1500);
    g_view.Acquire();
    SetPins(key);
    SetGroup(key);
    Pump(2500);
    Log(L"   values[%s]:%s", label, ReadPins(key).c_str());
    Uia(label);
}

static const GUID kOurFmt = {0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}};

// Our real schema (names, keys, labels) as version 1.0.1 writes it.
static std::wstring OurXml(const std::wstring& label = L"Pinned", const std::wstring& text = L"Pinned") {
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"utf-16\"?>\r\n"
        L"<schema xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\" schemaVersion=\"1.0\">\r\n"
        L"  <propertyDescriptionList publisher=\"ExplorerPinned\" product=\"ExplorerPinned\">\r\n";
    for (int pid = 2; pid <= 3; pid++) {
        xml += std::wstring(L"    <propertyDescription name=\"") + (pid == 2 ? L"ExplorerPinned.PinState" : L"ExplorerPinned.PinStateAlt") +
               L"\" formatID=\"" + GuidStr(kOurFmt) + L"\" propID=\"" + std::to_wstring(pid) + L"\">\r\n";
        xml += L"      <searchInfo inInvertedIndex=\"false\" isColumn=\"false\"/>\r\n"
               L"      <typeInfo type=\"UInt32\" isInnate=\"true\" isViewable=\"true\" groupingRange=\"Enumerated\"/>\r\n"
               L"      <labelInfo label=\"" + label + L"\"/>\r\n"
               L"      <displayInfo displayType=\"Enumerated\" defaultColumnWidth=\"12\">\r\n"
               L"        <enumeratedList>\r\n"
               L"          <enum name=\"Pinned\" value=\"0\" text=\"" + text + L"\"/>\r\n"
               L"        </enumeratedList>\r\n"
               L"      </displayInfo>\r\n"
               L"    </propertyDescription>\r\n";
    }
    xml += L"  </propertyDescriptionList>\r\n</schema>\r\n";
    return xml;
}

static const wchar_t kSchemaKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertySchema";

// Paths of the registered schemas named ExplorerPinned.propdesc.
static std::vector<std::pair<std::wstring, std::wstring>> OurEntries() {
    std::vector<std::pair<std::wstring, std::wstring>> result;  // subkey, path
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSchemaKey, 0, KEY_READ, &root) != ERROR_SUCCESS) return result;
    wchar_t sub[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wchar_t path[1024] = L"", uri[256] = L"";
        DWORD pl = sizeof(path), ul = sizeof(uri);
        RegGetValueW(root, sub, NULL, RRF_RT_REG_SZ, NULL, path, &pl);
        RegGetValueW(root, sub, L"URI", RRF_RT_REG_SZ, NULL, uri, &ul);
        if (_wcsicmp(uri, L"explorerpinned.propdesc") == 0) result.push_back({sub, path});
    }
    RegCloseKey(root);
    return result;
}


// ---------------- recovery variants ----------------
static void RunEp(const std::wstring& args, bool wait) {
    std::wstring cmd = L"\"" + g_epExe + L"\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Log(L"  CreateProcess failed %lu: %s", GetLastError(), cmd.c_str());
        return;
    }
    if (wait)
        while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) Pump(50);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Log(L"  ran %s -> %lu", args.c_str(), code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
static const PROPERTYKEY kNewKey = {{0xb1113708, 0x216c, 0x4097, {0x94, 0x00, 0xbd, 0x76, 0xbc, 0x4a, 0xfe, 0x7c}}, 2};
static std::wstring g_dir2;

static void Check(const wchar_t* label) {
    Navigate(L"C:\\Windows\\Help");
    Navigate(g_dir);
    g_view.fv->SetCurrentViewMode(FVM_DETAILS);
    std::wstring l = std::wstring(L"existing window ") + label;
    Try(kNewKey, l.c_str());
    View saved = g_view;
    g_view = View();
    if (OpenWindow(g_dir2)) {
        g_view.fv->SetCurrentViewMode(FVM_DETAILS);
        Pump(1000);
        std::wstring l2 = std::wstring(L"new window ") + label;
        Try(kNewKey, l2.c_str());
        g_view.Release();
        g_view.wb->Quit();
        Pump(1000);
    } else {
        Log(L"  new window not found");
    }
    g_view = saved;
    g_view.Acquire();
}

int wmain(int argc, wchar_t** argv) {
    g_t0 = GetTickCount();
    SetConsoleOutputCP(CP_UTF8);
    OleInitialize(NULL);
    wchar_t full[MAX_PATH];
    GetFullPathNameW(argv[1], MAX_PATH, full, NULL);
    g_epExe = full;
    std::wstring variant = argc > 2 ? argv[2] : L"P";
    Log(L"variant %s", variant.c_str());
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = LongPath(tmp);
    g_dir = base + L"ep_fix1";
    g_dir2 = base + L"ep_fix2";
    for (auto& d : {g_dir, g_dir2}) {
        SHCreateDirectoryExW(NULL, d.c_str(), NULL);
        for (const wchar_t* n : {L"\\alpha.txt", L"\\bravo.txt", L"\\charlie.txt", L"\\delta.txt"}) WriteBytes(d + n, "x", 1);
    }
    g_pins = {L"bravo.txt", L"delta.txt"};
    HKEY k;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    RegSetValueExW(k, L"Language", 0, REG_SZ, (const BYTE*)L"ja", 6);
    RegCloseKey(k);

    if (variant != L"R") {
        Log(L"######## seed: old schema registered from two folders, the first file deleted");
        std::wstring first = base + L"ep_old_portable\\ExplorerPinned.propdesc";
        std::wstring second = base + L"ep_old_installed\\ExplorerPinned.propdesc";
        std::wstring label = (variant == L"J1" || variant == L"J2") ? L"ピン止め" : L"Pinned";
        std::wstring text = (variant == L"J1" || variant == L"J3") ? L"ピン止め" : L"Pinned";
        WriteUtf16(first, OurXml(label, text));
        WriteUtf16(second, OurXml(label, text));
        Reg(first);
        Reg(second);
        DeleteFileW(first.c_str());
        RemoveDirectoryW((base + L"ep_old_portable").c_str());
        PSRefreshPropertySchema();
        DumpSchemas(L"seeded");
    }

    Log(L"######## setup");
    if (variant == L"S") {
        RunEp(L"register-schema --lang ja", true);  // no refresh from a process with an old view
    } else {
        RunEp(L"setup --no-startup --no-agent --quiet", true);
        int refreshes = variant == L"T" ? 3 : 1;
        for (int i = 0; i < refreshes; i++) {
            PSRefreshPropertySchema();
            Describe(kNewKey, L"right after setup");
        }
    }
    DumpSchemas(L"after setup");

    bool agent = variant != L"Q";
    if (agent) {
        RunEp(L"agent", false);
        Pump(1500);
        RunEp(L"pin \"" + g_dir + L"\\bravo.txt\" \"" + g_dir + L"\\delta.txt\"", true);
        RunEp(L"pin \"" + g_dir2 + L"\\bravo.txt\" \"" + g_dir2 + L"\\delta.txt\"", true);
    }
    if (!OpenWindow(g_dir)) {
        Log(L"window not found");
        return 1;
    }
    g_view.fv->SetCurrentViewMode(FVM_DETAILS);
    if (agent) {
        Pump(6000);
        Uia(L"agent, first window");
        Pump(4000);
        Uia(L"agent, first window later");
        View saved = g_view;
        g_view = View();
        if (OpenWindow(g_dir2)) {
            Pump(6000);
            Uia(L"agent, second window");
            g_view.Release();
            g_view.wb->Quit();
        }
        g_view = saved;
        g_view.Acquire();
        Navigate(L"C:\\Windows\\Help");
        Navigate(g_dir);
        Pump(6000);
        Uia(L"agent, first window after navigating");
        RunEp(L"exit", true);
        Pump(2000);
    }
    Check(L"probe groups");
    RunEp(L"register-schema --lang ja", true);
    Pump(2000);
    Check(L"probe groups after another registration");

    RunEp(L"uninstall --quiet", true);
    DumpSchemas(L"end");
    g_view.Release();
    g_view.wb->Quit();
    Log(L"done");
    OleUninitialize();
    return 0;
}
