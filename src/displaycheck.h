#pragma once

#include "common.h"

namespace ep {

// Diagnostics: reads, through UI Automation, which groups Explorer shows in the window and
// writes them to the log. Runs on a worker thread and returns at once.
void LogDisplayedGroups(HWND window, const std::wstring& folder);

}  // namespace ep
