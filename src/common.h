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
// {B1113708-216C-4097-9400-BD76BC4AFE7C}, 2 and 3
inline constexpr PROPERTYKEY kPinStateKeys[2] = {
    {{0xb1113708, 0x216c, 0x4097, {0x94, 0x00, 0xbd, 0x76, 0xbc, 0x4a, 0xfe, 0x7c}}, 2},
    {{0xb1113708, 0x216c, 0x4097, {0x94, 0x00, 0xbd, 0x76, 0xbc, 0x4a, 0xfe, 0x7c}}, 3},
};
inline constexpr const wchar_t* kPinStateNames[2] = {L"ExplorerPinned.Group.Pinned", L"ExplorerPinned.Group.PinnedAlt"};
inline constexpr wchar_t kPinStateFormatId[] = L"{B1113708-216C-4097-9400-BD76BC4AFE7C}";

// The schema describing those properties. Windows tells schemas apart by file name, so every
// copy of the program (installed or portable) registers the same file in one place.
inline constexpr wchar_t kSchemaFileName[] = L"ExplorerPinnedGroup.propdesc";
inline constexpr wchar_t kSchemaDirName[] = L"ExplorerPinned";  // under %ProgramData%

// Versions 1.0.0 and 1.0.1 registered ExplorerPinned.propdesc next to the program with these
// keys. A second copy of that file registered from another folder (a portable copy, say)
// made the registration fail partly, and Explorer then showed every item as "Unspecified".
// The old registrations are removed, and views Explorer saved with the old keys are still
// recognized as grouped by pins.
// {11AABE88-6952-4B06-9180-0BC4964E3158}, 2 and 3
inline constexpr PROPERTYKEY kLegacyPinStateKeys[2] = {
    {{0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}}, 2},
    {{0x11aabe88, 0x6952, 0x4b06, {0x91, 0x80, 0x0b, 0xc4, 0x96, 0x4e, 0x31, 0x58}}, 3},
};
inline constexpr const wchar_t* kLegacyPinStateNames[2] = {L"ExplorerPinned.PinState", L"ExplorerPinned.PinStateAlt"};
inline constexpr wchar_t kLegacyPinStateFormatId[] = L"{11AABE88-6952-4B06-9180-0BC4964E3158}";
inline constexpr wchar_t kLegacySchemaFileName[] = L"ExplorerPinned.propdesc";

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
