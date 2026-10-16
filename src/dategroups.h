#pragma once

#include "common.h"

#include <shobjidl.h>

#include <functional>

namespace ep {

// Below the pinned group, the other items of a folder that is grouped by date (such as
// Downloads) keep date groups: they get these values on the pin property, whose labels are those
// of Explorer's own date groups ("Today" ... "A long time ago"). Groups sort by value, so they
// follow the pinned group (kPinnedValue) in this order. Items of other folders get no value and
// show up in Explorer's "Unspecified" group.
inline constexpr UINT kDateGroupFirst = 1;  // Today
inline constexpr UINT kDateGroupCount = 8;  // Today ... A long time ago

// HKCU\Software\ExplorerPinned: DateGroups = "0" turns the date groups off.
bool DateGroupsSettingOn();
void SetDateGroupsSetting(bool on);

// True for a grouping by date modified, created or accessed: the date the other items are
// grouped by when the folder has pins.
bool IsGroupingByDate(const PROPERTYKEY& key);

// The date group of a time (UTC) as seen at `localNow` (from GetLocalTime).
UINT DateGroupOf(const FILETIME& utc, const SYSTEMTIME& localNow);

// A number that changes at local midnight: date groups written on another day are out of date.
DWORD DayNumber(const SYSTEMTIME& localNow);

// Date group values of the items of a view that are not pinned. `folder` is the folder of the
// view, used to read each item's name and date from its ID list (it can live in this process
// while the view is in Explorer's); `isPinned` tells pinned items by name.
struct DateGroupValues {
    IFolderView2* view;
    IShellFolder* folder;
    PROPERTYKEY dateKey;  // the date to group by; GUID_NULL clears the values instead
    std::function<bool(const std::wstring&)> isPinned;

    // Writes the values to `key`. Returns how many items got a date group.
    size_t Write(const PROPERTYKEY& key) const;
    // True when every item that is not pinned has the value it should have on `key`.
    bool Match(const PROPERTYKEY& key) const;
};

}  // namespace ep
