#pragma once

#include "common.h"

namespace ep {

// Pinned items are stored per user as value names under
// HKCU\Software\ExplorerPinned\Pins (the value name is the full path).
// Registry value names are case-insensitive, which matches NTFS semantics.
class PinStore {
public:
    static std::vector<std::wstring> LoadAll();
    static bool IsPinned(const std::wstring& path);
    static bool Add(const std::wstring& path);     // true if the pin was newly added
    static bool Remove(const std::wstring& path);  // true if a pin was removed
    static int RemoveMissing();                    // drops pins whose target no longer exists

    // Names (last path component) of pinned items directly inside `folder`.
    static std::vector<std::wstring> NamesInFolder(const std::vector<std::wstring>& pins, const std::wstring& folder);
};

}  // namespace ep
