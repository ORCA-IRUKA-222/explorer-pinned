#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <propsys.h>

#include <string>
#include <vector>

namespace ep {

// Properties used as the "group by" key in Explorer.
// A pinned item carries kPinnedValue in the view's property cache (IFolderView2::SetViewProperty);
// every other item has no value and falls into Explorer's "Unspecified" group, which sorts last.
//
// Explorer only re-groups items when the group-by key changes, so the agent writes the new
// values to the key that is not active and then switches to it. Both keys share the same label.
// {11AABE88-6952-4B06-9180-0BC4964E3158}, 2 and 3
inline constexpr PROPERTYKEY kPinStateKeys[2] = {
    {{0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}}, 2},
    {{0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}}, 3},
};
inline constexpr const wchar_t* kPinStateNames[2] = {L"ExplorerPinned.PinState", L"ExplorerPinned.PinStateAlt"};
inline constexpr wchar_t kPinStateFormatId[] = L"{11AABE88-6952-4B06-9180-0BC4964E3158}";
inline constexpr UINT kPinnedValue = 0;

inline constexpr wchar_t kRegRoot[] = L"Software\\ExplorerPinned";
inline constexpr wchar_t kRegPins[] = L"Software\\ExplorerPinned\\Pins";
inline constexpr wchar_t kRegPreviousGroupBy[] = L"Software\\ExplorerPinned\\PreviousGroupBy";
// Folders whose view Explorer may have saved while grouped by pins.
inline constexpr wchar_t kRegGroupedFolders[] = L"Software\\ExplorerPinned\\GroupedFolders";
inline constexpr wchar_t kRegRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kRunValueName[] = L"ExplorerPinned";

inline constexpr wchar_t kAgentMutexName[] = L"Local\\ExplorerPinned.Agent";
inline constexpr wchar_t kAgentWindowClass[] = L"ExplorerPinned.AgentWindow";

inline constexpr wchar_t kProjectUrl[] = L"https://github.com/ORCA-IRUKA-222/explorer-pinned";

// Messages understood by the agent window.
inline constexpr UINT kMsgReapply = WM_APP + 1;
inline constexpr UINT kMsgExit = WM_APP + 2;
inline constexpr UINT kMsgTray = WM_APP + 3;

}  // namespace ep
