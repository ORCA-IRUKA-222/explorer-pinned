// Research probe: pinned grouping inside file dialogs through the shell extension DLL.
// Usage: dialogprobe <ExplorerPinned.exe> <ExplorerPinnedShell.dll>
//        dialogprobe host <folder>   (shows an Open dialog on <folder>)
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

static DWORD g_t0;
static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    int p = swprintf_s(buf, L"%6lu ", GetTickCount() - g_t0);
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf + p, _countof(buf) - p, _TRUNCATE, fmt, ap);
    va_end(ap);
    char out[8192];
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL);
    if (n > 0) {
        fputs(out, stdout);
        fputs("\n", stdout);
        fflush(stdout);
    }
}

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

// ---------------- UIA
static HWND g_uiaHwnd;
static std::wstring g_uiaOut;
static DWORD WINAPI UiaThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    std::wstring out = L"(uia failed)";
    IUIAutomation* uia = NULL;
    CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    IUIAutomationElement* root = NULL;
    if (uia) uia->ElementFromHandle(g_uiaHwnd, &root);
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
        std::map<std::wstring, std::pair<int, std::wstring>> groups;
        for (int i = 0; i < len; i++) {
            IUIAutomationElement* el = NULL;
            arr->GetElement(i, &el);
            BSTR name = NULL, gname = NULL;
            el->get_CurrentName(&name);
            IUIAutomationElement* parent = NULL;
            walker->GetParentElement(el, &parent);
            CONTROLTYPEID type = 0;
            if (parent) {
                parent->get_CurrentControlType(&type);
                if (type == UIA_GroupControlTypeId) parent->get_CurrentName(&gname);
            }
            RECT r = {};
            el->get_CurrentBoundingRectangle(&r);
            std::wstring g = gname ? gname : L"(none)";
            auto it = groups.find(g);
            if (it == groups.end()) it = groups.emplace(g, std::make_pair((int)r.top, std::wstring())).first;
            it->second.first = std::min(it->second.first, (int)r.top);
            it->second.second += std::wstring(L" ") + (name ? name : L"");
            SysFreeString(name);
            SysFreeString(gname);
            if (parent) parent->Release();
            el->Release();
        }
        std::vector<std::pair<int, std::wstring>> order;
        for (auto& g : groups) order.push_back({g.second.first, g.first});
        std::sort(order.begin(), order.end());
        out.clear();
        for (auto& o : order) out += L"[" + o.second + L"]" + groups[o.second].second + L" ";
        if (out.empty()) out = L"(no items)";
        if (arr) arr->Release();
        if (walker) walker->Release();
        if (cond) cond->Release();
        root->Release();
    }
    if (uia) uia->Release();
    g_uiaOut = out;
    CoUninitialize();
    return 0;
}

static std::wstring Groups(HWND hwnd) {
    g_uiaHwnd = hwnd;
    HANDLE t = CreateThread(NULL, 0, UiaThread, NULL, 0, NULL);
    DWORD start = GetTickCount();
    while (WaitForSingleObject(t, 0) == WAIT_TIMEOUT && GetTickCount() - start < 30000) Pump(20);
    CloseHandle(t);
    return g_uiaOut;
}

// ---------------- helpers
struct FindReq {
    DWORD pid;
    HWND found;
};
static BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lp) {
    FindReq* r = (FindReq*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[32];
    if (pid == r->pid && IsWindowVisible(hwnd) && GetClassNameW(hwnd, cls, 32) && wcscmp(cls, L"#32770") == 0) {
        r->found = hwnd;
        return FALSE;
    }
    return TRUE;
}
static HWND WaitDialog(DWORD pid, DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        FindReq r = {pid, NULL};
        EnumWindows(FindDialogProc, (LPARAM)&r);
        if (r.found) return r.found;
        Pump(200);
    }
    return NULL;
}

static PROCESS_INFORMATION Start(const std::wstring& cmd, bool noDialogSupport = false) {
    std::wstring c = cmd;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (noDialogSupport) SetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", L"1");
    if (!CreateProcessW(NULL, c.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) Log(L"CreateProcess failed: %s", cmd.c_str());
    SetEnvironmentVariableW(L"EXPLORERPINNED_NO_DIALOG", NULL);
    return pi;
}

static void WriteFileText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    WriteFile(h, "x", 1, &w, NULL);
    CloseHandle(h);
}

static void Watch(HWND dlg, DWORD ms, const wchar_t* label) {
    std::wstring last;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        std::wstring g = Groups(dlg);
        if (g != last) Log(L"  [%s] %s", label, g.c_str());
        last = g;
        Pump(700);
    }
}

static void CloseDialog(HWND dlg, PROCESS_INFORMATION& pi) {
    PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
    Pump(1500);
    if (pi.hProcess) {
        WaitForSingleObject(pi.hProcess, 5000);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pi = {};
    }
}

static int Host(const std::wstring& folder) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* d = NULL;
    CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    IShellItem* item = NULL;
    SHCreateItemFromParsingName(folder.c_str(), NULL, IID_PPV_ARGS(&item));
    if (d && item) {
        d->SetFolder(item);
        d->Show(NULL);
    }
    return 0;
}

static std::wstring g_exe;
static void RunExe(const std::wstring& args) {
    PROCESS_INFORMATION pi = Start(L"\"" + g_exe + L"\" " + args);
    if (pi.hProcess) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

int wmain(int argc, wchar_t** argv) {
    g_t0 = GetTickCount();
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && wcscmp(argv[1], L"host") == 0) return Host(argv[2]);
    OleInitialize(NULL);
    wchar_t full[MAX_PATH];
    GetFullPathNameW(argv[1], MAX_PATH, full, NULL);
    g_exe = full;
    GetFullPathNameW(argv[2], MAX_PATH, full, NULL);
    std::wstring dll = full;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    HKEY k;
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    RegSetValueExW(k, L"Language", 0, REG_SZ, (const BYTE*)L"ja", 6);
    RegCloseKey(k);
    RunExe(L"setup --no-startup --no-agent --quiet");

    HMODULE m = LoadLibraryW(dll.c_str());
    typedef HRESULT(STDAPICALLTYPE * Fn)();
    Fn reg = m ? (Fn)GetProcAddress(m, "DllRegisterServer") : NULL;
    Log(L"DllRegisterServer: 0x%08lx", reg ? (unsigned long)reg() : 0xffffffffUL);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetLongPathNameW(tmp, tmp, MAX_PATH);
    std::wstring dir = std::wstring(tmp) + L"ep_dialog";
    SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
    for (const wchar_t* n : {L"\\alpha.txt", L"\\bravo.txt", L"\\charlie.txt", L"\\echo.txt"}) WriteFileText(dir + n);
    CreateDirectoryW((dir + L"\\delta").c_str(), NULL);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned\\Pins", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    for (const wchar_t* n : {L"\\bravo.txt", L"\\delta"}) {
        std::wstring p = dir + n;
        RegSetValueExW(k, p.c_str(), 0, REG_SZ, (const BYTE*)L"x", 4);
    }
    RegCloseKey(k);

    Log(L"######## D1 an Open dialog of a test program");
    PROCESS_INFORMATION host = Start(L"\"" + std::wstring(self) + L"\" host \"" + dir + L"\"");
    HWND dlg = WaitDialog(host.dwProcessId, 15000);
    Log(L"  dialog %p", dlg);
    if (dlg) {
        Watch(dlg, 12000, L"D1");
        Log(L"  -- unpin bravo");
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned\\Pins", 0, KEY_SET_VALUE, &k);
        RegDeleteValueW(k, (dir + L"\\bravo.txt").c_str());
        RegCloseKey(k);
        Watch(dlg, 5000, L"D1 unpin");
        CloseDialog(dlg, host);
    }

    Log(L"######## D2 the same folder again without dialog support (what Explorer saved for the dialog)");
    host = Start(L"\"" + std::wstring(self) + L"\" host \"" + dir + L"\"", true);
    dlg = WaitDialog(host.dwProcessId, 15000);
    if (dlg) {
        Watch(dlg, 4000, L"D2");
        CloseDialog(dlg, host);
    }

    Log(L"######## D3 Notepad's Open dialog");
    PROCESS_INFORMATION np = Start(L"notepad.exe");
    Pump(3000);
    HWND main = NULL;
    for (int i = 0; i < 20 && !main; i++) {
        struct R {
            DWORD pid;
            HWND w;
        } r = {np.dwProcessId, NULL};
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                R* r = (R*)lp;
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid == r->pid && IsWindowVisible(h) && GetMenu(h)) {
                    r->w = h;
                    return FALSE;
                }
                return TRUE;
            },
            (LPARAM)&r);
        main = r.w;
        if (!main) Pump(500);
    }
    Log(L"  notepad window %p", main);
    if (main) {
        HMENU file = GetSubMenu(GetMenu(main), 0);
        UINT openId = file ? GetMenuItemID(file, 1) : 0;
        Log(L"  open command %u", openId);
        PostMessageW(main, WM_COMMAND, openId, 0);
        dlg = WaitDialog(np.dwProcessId, 15000);
        Log(L"  dialog %p", dlg);
        if (dlg) {
            Pump(1500);
            HWND combo = GetDlgItem(dlg, 1148);
            HWND edit = combo ? FindWindowExW(combo, NULL, L"ComboBox", NULL) : NULL;
            if (edit) edit = FindWindowExW(edit, NULL, L"Edit", NULL);
            if (!edit && combo) edit = FindWindowExW(combo, NULL, L"Edit", NULL);
            Log(L"  file name box %p / %p", combo, edit);
            if (edit) {
                SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)dir.c_str());
                PostMessageW(dlg, WM_COMMAND, IDOK, 0);
            }
            Watch(dlg, 12000, L"D3");
            PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        }
    }
    if (np.hProcess) {
        TerminateProcess(np.hProcess, 0);
        CloseHandle(np.hProcess);
        CloseHandle(np.hThread);
    }

    Fn unreg = m ? (Fn)GetProcAddress(m, "DllUnregisterServer") : NULL;
    if (unreg) unreg();
    Log(L"done");
    return 0;
}
