#pragma once

#include "common.h"

namespace ep {

// Right-click menu entries (per user, HKCU\Software\Classes).
// The "Pin"/"Unpin" entries are shown only where they apply by keeping their
// AppliesTo conditions in sync with the pin list.
void UpdateContextMenu(const std::vector<std::wstring>& pins);
void RemoveContextMenu();
bool IsContextMenuRegistered();

// Start-up entry (HKCU Run).
bool IsStartupEnabled();
void SetStartupEnabled(bool enabled);

// Property description that gives the "Pinned" group its label.
// Registration writes to HKLM and therefore needs administrator rights.
bool IsSchemaRegistered();
HRESULT RegisterSchema();    // also removes earlier registrations of this program
HRESULT UnregisterSchema();
std::wstring SchemaPath();   // %ProgramData%\ExplorerPinned\ExplorerPinnedGroup.propdesc

struct SchemaRegistration {
    std::wstring path;
    bool legacy;  // ExplorerPinned.propdesc of version 1.0.x
    bool exists;
};
std::vector<SchemaRegistration> SchemaRegistrations();

// Runs this executable elevated with `args` and waits for it. Returns its exit code (or -1).
int RunElevated(const std::wstring& args);

}  // namespace ep
