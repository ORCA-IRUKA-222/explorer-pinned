#include "buttons.h"

#include <memory>
#include <set>

#include "toggle.h"
#include "util.h"

namespace ep {
namespace {

constexpr UINT kMsgState = WM_APP + 1;  // the agent sent a new state
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr UINT_PTR kTimerCheck = 1;
constexpr UINT kCheckMs = 500;  // also looks every half second, for events that were missed

}  // namespace

struct ExplorerButtons::Thread {
    // Written by the agent's thread.
    SRWLOCK lock = SRWLOCK_INIT;
    Frames frames;
    bool on = true;
    bool show = true;
    HANDLE handle = nullptr;
    HANDLE ready = nullptr;
    HWND window = nullptr;  // message-only window of the button thread

    // Only used on the button thread.
    Frames current;
    bool currentOn = true;
    bool currentShow = true;
    std::map<HWND, std::unique_ptr<ToggleButton>> buttons;  // per Explorer window
    std::map<DWORD, std::vector<HWINEVENTHOOK>> hooks;     // per Explorer process
    std::set<HWND> logged;

    static DWORD WINAPI Main(void* param);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    void TakeState();
    void HookProcesses();
    void Refresh();
    void Refresh(HWND frame);
    void Click();
};

namespace {
// The window event callback has no context; there is one button thread.
ExplorerButtons::Thread* g_thread = nullptr;
}  // namespace

DWORD WINAPI ExplorerButtons::Thread::Main(void* param) {
    auto* t = static_cast<Thread*>(param);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ExplorerPinned.Buttons";
    RegisterClassExW(&wc);
    t->window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (t->window) {
        SetWindowLongPtrW(t->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t));
        SetTimer(t->window, kTimerCheck, kCheckMs, nullptr);
        g_thread = t;
    }
    SetEvent(t->ready);
    if (!t->window) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    for (auto& entry : t->hooks)
        for (HWINEVENTHOOK h : entry.second) UnhookWinEvent(h);
    t->hooks.clear();
    t->buttons.clear();
    g_thread = nullptr;
    DestroyWindow(t->window);
    return 0;
}

LRESULT CALLBACK ExplorerButtons::Thread::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* t = reinterpret_cast<Thread*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case kMsgState:
            if (t) {
                t->TakeState();
                t->Refresh();
            }
            return 0;
        case WM_TIMER:
            if (t) t->Refresh();
            return 0;
        case kMsgQuit:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ExplorerButtons::Thread::TakeState() {
    AcquireSRWLockExclusive(&lock);
    current = frames;
    currentOn = on;
    currentShow = show;
    ReleaseSRWLockExclusive(&lock);
    for (auto it = buttons.begin(); it != buttons.end();)
        it = current.count(it->first) ? std::next(it) : buttons.erase(it);
    HookProcesses();
}

// Window events of the Explorer processes: moves, size changes, tab switches, minimizing and
// closing. They arrive on this thread as messages.
void ExplorerButtons::Thread::HookProcesses() {
    std::set<DWORD> pids;
    for (const auto& entry : current) {
        DWORD pid = 0;
        GetWindowThreadProcessId(entry.first, &pid);
        if (pid) pids.insert(pid);
    }
    for (auto it = hooks.begin(); it != hooks.end();) {
        if (pids.count(it->first)) {
            ++it;
            continue;
        }
        for (HWINEVENTHOOK h : it->second) UnhookWinEvent(h);
        it = hooks.erase(it);
    }
    const DWORD ranges[][2] = {{EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
                               {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
                               {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}};
    for (DWORD pid : pids) {
        if (hooks.count(pid)) continue;
        auto& list = hooks[pid];
        for (const auto& r : ranges)
            if (HWINEVENTHOOK h = SetWinEventHook(r[0], r[1], nullptr, OnWinEvent, pid, 0, WINEVENT_OUTOFCONTEXT))
                list.push_back(h);
    }
}

void CALLBACK ExplorerButtons::Thread::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                                  DWORD, DWORD) {
    Thread* t = g_thread;
    if (!t || !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY) {
        t->buttons.erase(hwnd);  // its Explorer window closed
        return;
    }
    HWND frame = GetAncestor(hwnd, GA_ROOT);
    if (!frame || !t->current.count(frame)) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        auto b = t->buttons.find(frame);
        if (b != t->buttons.end() && b->second->Visible()) b->second->Place();
        return;
    }
    t->Refresh(frame);
}

void ExplorerButtons::Thread::Refresh() {
    for (const auto& entry : current) Refresh(entry.first);
}

void ExplorerButtons::Thread::Refresh(HWND frame) {
    auto it = current.find(frame);
    if (it == current.end()) return;
    // The tab on display (the folder views of the other tabs are hidden).
    HWND view = nullptr;
    bool pins = false;
    for (const Tab& tab : it->second) {
        if (IsWindowVisible(tab.view)) {
            view = tab.view;
            pins = tab.pins;
            break;
        }
    }
    bool alive = IsWindow(frame) != FALSE;
    bool visible = alive && currentShow && view && pins && IsWindowVisible(frame) && !IsIconic(frame);
    auto b = buttons.find(frame);
    if (!visible) {
        if (b != buttons.end()) {
            if (alive)
                b->second->Hide();
            else
                buttons.erase(b);
        }
        return;
    }
    if (b == buttons.end()) {
        auto button = std::make_unique<ToggleButton>();
        button->onClick = [this] { Click(); };
        b = buttons.emplace(frame, std::move(button)).first;
    }
    b->second->Show(frame, view, currentOn);
    if (logged.insert(frame).second && b->second->Hwnd()) {
        RECT r, v;
        GetWindowRect(b->second->Hwnd(), &r);
        GetWindowRect(view, &v);
        LogLine(L"button at %ld,%ld-%ld,%ld (folder view %ld,%ld-%ld,%ld)", r.left, r.top, r.right, r.bottom, v.left, v.top,
                v.right, v.bottom);
    }
}

void ExplorerButtons::Thread::Click() {
    bool turnOn = !PinsEnabled();
    SetPinsEnabled(turnOn);  // the agent follows through its registry watch
    LogLine(L"pinned group turned %s with the button", turnOn ? L"on" : L"off");
    currentOn = turnOn;
    Refresh();
}

ExplorerButtons::ExplorerButtons() = default;

ExplorerButtons::~ExplorerButtons() { Stop(); }

void ExplorerButtons::Start() {
    if (thread_) return;
    auto* t = new Thread();
    t->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->handle = CreateThread(nullptr, 0, Thread::Main, t, 0, nullptr);
    if (!t->handle || !t->ready) {
        if (t->handle) CloseHandle(t->handle);
        if (t->ready) CloseHandle(t->ready);
        delete t;
        return;
    }
    WaitForSingleObject(t->ready, 5000);
    thread_ = t;
    sentOnce_ = false;
}

void ExplorerButtons::Stop() {
    if (!thread_) return;
    Thread* t = thread_;
    thread_ = nullptr;
    if (t->window) PostMessageW(t->window, kMsgQuit, 0, 0);
    bool ended = WaitForSingleObject(t->handle, 5000) == WAIT_OBJECT_0;
    CloseHandle(t->handle);
    CloseHandle(t->ready);
    if (ended) delete t;  // else the thread may still use it
}

void ExplorerButtons::Update(const Frames& frames, bool on, bool show) {
    if (!thread_ || !thread_->window) return;
    if (sentOnce_ && frames == sent_ && on == sentOn_ && show == sentShow_) return;
    sent_ = frames;
    sentOn_ = on;
    sentShow_ = show;
    sentOnce_ = true;
    AcquireSRWLockExclusive(&thread_->lock);
    thread_->frames = frames;
    thread_->on = on;
    thread_->show = show;
    ReleaseSRWLockExclusive(&thread_->lock);
    PostMessageW(thread_->window, kMsgState, 0, 0);
}

}  // namespace ep
