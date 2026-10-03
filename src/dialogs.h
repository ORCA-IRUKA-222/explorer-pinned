#pragma once

#include "common.h"

namespace ep {

// File dialogs (Open/Save, and the upload dialogs of web browsers) show the pinned group
// through ExplorerPinnedShell.dll. It is registered as an icon overlay handler, which makes
// Windows load it into every program that shows a file dialog.
inline constexpr wchar_t kShellDllName[] = L"ExplorerPinnedShell.dll";      // 64-bit programs
inline constexpr wchar_t kShellDll32Name[] = L"ExplorerPinnedShell32.dll";  // 32-bit programs

// `view` is KEY_WOW64_64KEY (64-bit programs) or KEY_WOW64_32KEY (32-bit programs). Needs
// administrator rights.
bool RegisterDialogExtension(REGSAM view, const std::wstring& dllPath);
void UnregisterDialogExtension(REGSAM view);
// The registered DLL, or empty.
std::wstring RegisteredDialogExtension(REGSAM view);

// The DLL is loaded into every program that shows a file dialog, including programs that
// run as administrator: it must be in a folder that only administrators can change.
bool IsProtectedLocation(const std::wstring& path);

// On unless HKCU\Software\ExplorerPinned has Dialogs = "0".
bool DialogsSettingOn();
void SetDialogsSetting(bool on);

// File dialogs save their grouping per folder (like Explorer windows). Puts saved groupings
// by the pinned group back to no grouping; such a grouping is left behind only when a
// program ended while it showed a pinned folder. Returns how many were reset.
int ResetSavedPinnedGroupings();

}  // namespace ep
