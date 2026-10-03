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

// Prints the view-state bags that mention the pinned property.
static void DumpBags(HKEY root, const std::wstring& path, int depth) {
    HKEY k;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    std::wstring lines;
    bool ours = false;
    wchar_t name[512];
    BYTE data[4096];
    for (DWORD i = 0;; i++) {
        DWORD nl = 512, dl = sizeof(data), type = 0;
        if (RegEnumValueW(k, i, name, &nl, NULL, &type, data, &dl) != ERROR_SUCCESS) break;
        wchar_t line[1200];
        if (type == REG_SZ) {
            std::wstring v((wchar_t*)data);
            if (_wcsnicmp(v.c_str(), L"{B1113708", 9) == 0) ours = true;
            swprintf_s(line, L"    %s = %s", name, v.c_str());
        } else if (type == REG_DWORD) {
            swprintf_s(line, L"    %s = %lu", name, *(DWORD*)data);
        } else {
            swprintf_s(line, L"    %s (type %lu, %lu bytes)", name, type, dl);
        }
        lines += std::wstring(line) + L"\n";
    }
    if (ours) Log(L"  bag %s\n%s", path.c_str(), lines.c_str());
    if (depth < 6) {
        for (DWORD i = 0;; i++) {
            DWORD nl = 512;
            if (RegEnumKeyExW(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            DumpBags(root, path + L"\\" + name, depth + 1);
        }
    }
    RegCloseKey(k);
}

// Types a path into the dialog's file name box and presses Open: the dialog shows that folder.
static void NavigateDialog(HWND dlg, const std::wstring& path) {
    HWND combo = GetDlgItem(dlg, 1148);
    HWND edit = combo ? FindWindowExW(combo, NULL, L"ComboBox", NULL) : NULL;
    if (edit) edit = FindWindowExW(edit, NULL, L"Edit", NULL);
    if (!edit && combo) edit = FindWindowExW(combo, NULL, L"Edit", NULL);
    Log(L"  navigate to %s (file name box %p / %p)", path.c_str(), combo, edit);
    if (!edit) return;
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)path.c_str());
    PostMessageW(dlg, WM_COMMAND, IDOK, 0);
}

static std::wstring ImageName(DWORD pid) {
    std::wstring result;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return result;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        result = slash ? slash + 1 : path;
    }
    CloseHandle(h);
    return result;
}

static BOOL CALLBACK HasDefViewProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"SHELLDLL_DefView") == 0) {
        *(bool*)lp = true;
        return FALSE;
    }
    return TRUE;
}

struct FindByImage {
    const wchar_t* exe;
    const wchar_t* title;  // NULL: a file dialog
    HWND found;
};
static BOOL CALLBACK FindByImageProc(HWND hwnd, LPARAM lp) {
    FindByImage* r = (FindByImage*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (_wcsicmp(ImageName(pid).c_str(), r->exe) != 0) return TRUE;
    if (r->title) {
        wchar_t text[256] = L"";
        GetWindowTextW(hwnd, text, 256);
        if (!wcsstr(text, r->title)) return TRUE;
    } else {
        wchar_t cls[32];
        if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"#32770") != 0) return TRUE;
        bool defView = false;
        EnumChildWindows(hwnd, HasDefViewProc, (LPARAM)&defView);
        if (!defView) return TRUE;
    }
    r->found = hwnd;
    return FALSE;
}
static HWND WaitWindowOf(const wchar_t* exe, const wchar_t* title, DWORD ms) {
    DWORD start = GetTickCount();
    do {
        FindByImage r = {exe, title, NULL};
        EnumWindows(FindByImageProc, (LPARAM)&r);
        if (r.found) return r.found;
        Pump(250);
    } while (GetTickCount() - start < ms);
    return NULL;
}

// Clicks the page's file button through UI Automation (a click by assistive technology
// counts as a user action, which a page needs to open the file chooser). Runs on its own
// thread: a browser may only return from the click once its dialog closes.
static HWND g_clickHwnd;
static DWORD WINAPI ClickThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation* uia = NULL;
    CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    IUIAutomationElement* root = NULL;
    if (uia) uia->ElementFromHandle(g_clickHwnd, &root);
    VARIANT v;
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"pickfile");
    IUIAutomationCondition* cond = NULL;
    if (uia) uia->CreatePropertyCondition(UIA_NamePropertyId, v, &cond);
    IUIAutomationElement* el = NULL;
    for (int i = 0; i < 40 && root && cond && !el; i++) {
        root->FindFirst(TreeScope_Descendants, cond, &el);
        if (!el) Sleep(500);
    }
    HRESULT hr = E_FAIL;
    if (el) {
        IUIAutomationInvokePattern* inv = NULL;
        el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv));
        if (inv) {
            Log(L"  UIA: invoking the file button");
            hr = inv->Invoke();
            inv->Release();
        } else {
            IUIAutomationLegacyIAccessiblePattern* acc = NULL;
            el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc));
            if (acc) {
                Log(L"  UIA: default action on the file button");
                hr = acc->DoDefaultAction();
                acc->Release();
            }
        }
        el->Release();
    } else {
        Log(L"  UIA: file button not found");
    }
    Log(L"  UIA click: 0x%08lx", (unsigned long)hr);
    VariantClear(&v);
    if (cond) cond->Release();
    if (root) root->Release();
    if (uia) uia->Release();
    CoUninitialize();
    return 0;
}

static void PressSpace(HWND hwnd) {
    // Windows lets a process take the foreground right after a key press.
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(hwnd);
    Pump(500);
    Log(L"  keyboard: foreground %s", GetForegroundWindow() == hwnd ? L"ok" : L"not the browser");
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_SPACE;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
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

    std::wstring hostCmd = L"\"" + std::wstring(self) + L"\" host \"" + dir + L"\"";
    auto check = [&](const wchar_t* label) {
        PROCESS_INFORMATION h = Start(hostCmd, true);
        HWND d = WaitDialog(h.dwProcessId, 15000);
        if (d) {
            Watch(d, 3000, label);
            CloseDialog(d, h);
        }
    };
    auto setPin = [&](const wchar_t* name, bool on) {
        HKEY pk;
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerPinned\\Pins", 0, NULL, 0, KEY_SET_VALUE, NULL, &pk, NULL);
        std::wstring p = dir + L"\\" + name;
        if (on)
            RegSetValueExW(pk, p.c_str(), 0, REG_SZ, (const BYTE*)L"x", 4);
        else
            RegDeleteValueW(pk, p.c_str());
        RegCloseKey(pk);
    };

    Log(L"######## D1 an Open dialog of a test program, closed with a posted Cancel");
    PROCESS_INFORMATION host = Start(hostCmd);
    HWND dlg = WaitDialog(host.dwProcessId, 15000);
    Log(L"  dialog %p", dlg);
    if (dlg) {
        Watch(dlg, 8000, L"D1");
        Log(L"  -- unpin bravo");
        setPin(L"bravo.txt", false);
        Watch(dlg, 4000, L"D1 unpin");
        setPin(L"bravo.txt", true);
        Watch(dlg, 4000, L"D1 pin again");
        CloseDialog(dlg, host);
    }
    check(L"D1 saved");

    Log(L"######## D6 navigating to another folder inside the dialog, then Cancel");
    host = Start(hostCmd);
    dlg = WaitDialog(host.dwProcessId, 15000);
    if (dlg) {
        Watch(dlg, 6000, L"D6");
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        NavigateDialog(dlg, win);
        Pump(3000);
        CloseDialog(dlg, host);
    }
    check(L"D6 saved");
    DumpBags(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags", 0);

    // Upload dialogs of web browsers.
    std::wstring page = std::wstring(tmp) + L"ep_upload.html";
    {
        HANDLE h = CreateFileW(page.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        const char html[] = "<!doctype html><title>ep-upload</title><input type=file id=f aria-label=pickfile autofocus>";
        DWORD w;
        WriteFile(h, html, sizeof(html) - 1, &w, NULL);
        CloseHandle(h);
    }
    std::wstring url = L"file:///" + page;
    std::replace(url.begin(), url.end(), L'\\', L'/');
    struct Browser {
        const wchar_t* name;
        const wchar_t* exe;
        std::vector<std::wstring> paths;
    };
    wchar_t pf[MAX_PATH], pf86[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%", pf86, MAX_PATH);
    std::vector<Browser> browsers = {
        {L"Chrome", L"chrome.exe",
         {std::wstring(pf) + L"\\Google\\Chrome\\Application\\chrome.exe",
          std::wstring(pf86) + L"\\Google\\Chrome\\Application\\chrome.exe"}},
        {L"Edge", L"msedge.exe",
         {std::wstring(pf86) + L"\\Microsoft\\Edge\\Application\\msedge.exe",
          std::wstring(pf) + L"\\Microsoft\\Edge\\Application\\msedge.exe"}},
        {L"Firefox", L"firefox.exe", {std::wstring(pf) + L"\\Mozilla Firefox\\firefox.exe"}},
    };
    for (auto& b : browsers) {
        Log(L"######## %s upload dialog", b.name);
        std::wstring exe;
        for (auto& p : b.paths)
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) exe = p;
        if (exe.empty()) {
            Log(L"  not installed");
            continue;
        }
        std::wstring profile = std::wstring(tmp) + L"ep_profile_" + b.name;
        SHCreateDirectoryExW(NULL, profile.c_str(), NULL);
        std::wstring cmd = L"\"" + exe + L"\" ";
        if (wcscmp(b.exe, L"firefox.exe") == 0) {
            // No welcome pages in the fresh profile.
            HANDLE h = CreateFileW((profile + L"\\user.js").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            const char prefs[] = "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
                                 "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
                                 "user_pref(\"datareporting.policy.firstRunURL\", \"\");\n"
                                 "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n";
            DWORD w;
            WriteFile(h, prefs, sizeof(prefs) - 1, &w, NULL);
            CloseHandle(h);
        }
        if (wcscmp(b.exe, L"firefox.exe") == 0)
            cmd += L"-no-remote -profile \"" + profile + L"\" \"" + url + L"\"";
        else
            cmd += L"--user-data-dir=\"" + profile +
                   L"\" --no-first-run --no-default-browser-check --disable-search-engine-choice-screen "
                   L"--force-renderer-accessibility \"" + url + L"\"";
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
            Log(L"  CreateProcess failed %lu", GetLastError());
            CloseHandle(job);
            continue;
        }
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        HWND win = WaitWindowOf(b.exe, L"ep-upload", 30000);
        Log(L"  browser window %p", win);
        HWND bdlg = NULL;
        if (win) {
            Pump(2000);
            g_clickHwnd = win;
            CloseHandle(CreateThread(NULL, 0, ClickThread, NULL, 0, NULL));
            bdlg = WaitWindowOf(b.exe, NULL, 25000);
            if (!bdlg) {
                PressSpace(win);
                bdlg = WaitWindowOf(b.exe, NULL, 10000);
            }
        }
        if (bdlg) {
            DWORD dpid = 0;
            GetWindowThreadProcessId(bdlg, &dpid);
            Log(L"  dialog %p in process %lu (browser process %lu)", bdlg, dpid, pi.dwProcessId);
            Pump(1500);
            Watch(bdlg, 2500, b.name);
            NavigateDialog(bdlg, dir);
            Watch(bdlg, 10000, b.name);
            PostMessageW(bdlg, WM_COMMAND, IDCANCEL, 0);
            Pump(2000);
        } else {
            Log(L"  no file dialog");
        }
        TerminateJobObject(job, 0);
        CloseHandle(job);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Pump(2000);
    }

    Fn unreg = m ? (Fn)GetProcAddress(m, "DllUnregisterServer") : NULL;
    if (unreg) unreg();
    Log(L"done");
    return 0;
}
