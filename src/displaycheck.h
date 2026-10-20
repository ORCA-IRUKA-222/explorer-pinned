#pragma once

#include "common.h"

namespace ep {

// What Explorer shows in a folder window, read through UI Automation.
enum class Display {
    Shown,     // the pinned group is the topmost group
    NotShown,  // the list is scrolled to the top and starts with another group
    Unknown,   // no groups to see (still drawing, hidden tab, ...) or scrolled down
};

// Sent to `notify` when a check is done: wParam = the cookie, lParam = (LPARAM)Display.
struct DisplayCheck {
    HWND window;        // the folder view's window (or the Explorer window)
    std::wstring folder;  // for the log
    DWORD delayMs;      // time for Explorer to draw before reading
    HWND notify;
    UINT message;
    WPARAM cookie;
};

// Reads the groups on a worker thread, writes them to the log and posts the result.
void CheckDisplayedGroups(const DisplayCheck& check);

}  // namespace ep
