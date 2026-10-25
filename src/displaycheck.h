#pragma once

#include "common.h"

namespace ep {

enum class DisplayResult { Unknown, Shown, Missing };

// Reads, through UI Automation, which groups Explorer shows in the window, writes them to the
// log and posts `message` to `notify` (wParam: DisplayResult, lParam: `cookie`): Shown when the
// topmost group is `pinnedLabel`, Missing when another group is on top. Runs on a worker
// thread and returns at once.
void CheckDisplayedGroups(HWND window, const std::wstring& folder, const std::wstring& pinnedLabel, HWND notify,
                          UINT message, LPARAM cookie);

}  // namespace ep
