#include "dategroups.h"

#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>

#include "util.h"

namespace ep {
namespace {

// {B725F130-47EF-101A-A5F1-02608C9EEBAC}: System.DateModified (14), DateCreated (15), DateAccessed (16)
constexpr GUID kStorageFormat = {0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}};
constexpr DWORD kPidDateModified = 14;
constexpr DWORD kPidDateCreated = 15;
constexpr DWORD kPidDateAccessed = 16;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // SetViewProperty/GetViewProperty: see agent.cpp
#endif
HRESULT SetViewValue(IFolderView2* view, PCUITEMID_CHILD child, const PROPERTYKEY& key, const PROPVARIANT& value) {
    return view->SetViewProperty(child, key, value);
}
HRESULT GetViewValue(IFolderView2* view, PCUITEMID_CHILD child, const PROPERTYKEY& key, PROPVARIANT* value) {
    return view->GetViewProperty(child, key, value);
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

LONGLONG DayIndex(const SYSTEMTIME& st) {
    SYSTEMTIME day = {};
    day.wYear = st.wYear;
    day.wMonth = st.wMonth;
    day.wDay = st.wDay;
    FILETIME ft;
    if (!SystemTimeToFileTime(&day, &ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (LONGLONG)(u.QuadPart / 864000000000ULL);  // 100 ns units per day
}

// Days from the first day of the week (as set for the user) to `localNow`.
int DaysIntoWeek(const SYSTEMTIME& localNow) {
    DWORD first = 6;  // LOCALE_IFIRSTDAYOFWEEK: 0 = Monday ... 6 = Sunday
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                    reinterpret_cast<LPWSTR>(&first), sizeof(first) / sizeof(wchar_t));
    int mondayBased = (localNow.wDayOfWeek + 6) % 7;  // wDayOfWeek: 0 = Sunday
    return (mondayBased - (int)(first % 7) + 7) % 7;
}

std::wstring ItemName(IShellFolder* folder, PCUITEMID_CHILD child) {
    STRRET sr;
    wchar_t buf[MAX_PATH * 2];
    if (FAILED(folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr)) ||
        FAILED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
        return std::wstring();
    return buf;
}

bool ItemDate(IShellFolder* folder, PCUITEMID_CHILD child, const PROPERTYKEY& dateKey, FILETIME* utc) {
    WIN32_FIND_DATAW fd = {};
    if (FAILED(SHGetDataFromIDListW(folder, child, SHGDFIL_FINDDATA, &fd, sizeof(fd)))) return false;
    *utc = dateKey.pid == kPidDateCreated ? fd.ftCreationTime : dateKey.pid == kPidDateAccessed ? fd.ftLastAccessTime
                                                                                                  : fd.ftLastWriteTime;
    return utc->dwLowDateTime || utc->dwHighDateTime;
}

// Calls `each` with every item of the view and the value it should have (0 = no date group).
template <typename F>
void ForEachItem(const DateGroupValues& v, F each) {
    int count = 0;
    if (FAILED(v.view->ItemCount(SVGIO_ALLVIEW, &count))) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    bool dates = !IsEqualGUID(v.dateKey.fmtid, GUID_NULL);
    for (int i = 0; i < count; i++) {
        PITEMID_CHILD child = nullptr;
        if (FAILED(v.view->Item(i, &child)) || !child) continue;
        std::wstring name = ItemName(v.folder, child);
        if (!name.empty() && !v.isPinned(name)) {
            FILETIME ft;
            UINT group = dates && ItemDate(v.folder, child, v.dateKey, &ft) ? DateGroupOf(ft, now) : 0;
            each(child, group);
        }
        CoTaskMemFree(child);
    }
}

}  // namespace

bool DateGroupsSettingOn() {
    std::wstring v;
    return !(RegReadString(HKEY_CURRENT_USER, kRegRoot, L"DateGroups", &v) && v == L"0");
}

void SetDateGroupsSetting(bool on) { RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"DateGroups", on ? L"1" : L"0"); }

bool IsGroupingByDate(const PROPERTYKEY& key) {
    return IsEqualGUID(key.fmtid, kStorageFormat) &&
           (key.pid == kPidDateModified || key.pid == kPidDateCreated || key.pid == kPidDateAccessed);
}

UINT DateGroupOf(const FILETIME& utc, const SYSTEMTIME& localNow) {
    SYSTEMTIME u, item;
    if (!FileTimeToSystemTime(&utc, &u) || !SystemTimeToTzSpecificLocalTime(nullptr, &u, &item))
        return kDateGroupFirst + kDateGroupCount - 1;
    LONGLONG today = DayIndex(localNow), day = DayIndex(item);
    UINT group;
    if (day >= today)
        group = 0;  // today (and dates in the future)
    else if (day == today - 1)
        group = 1;  // yesterday
    else if (day >= today - DaysIntoWeek(localNow))
        group = 2;  // earlier this week
    else if (day >= today - DaysIntoWeek(localNow) - 7)
        group = 3;  // last week
    else if (item.wYear == localNow.wYear && item.wMonth == localNow.wMonth)
        group = 4;  // earlier this month
    else if ((localNow.wYear * 12 + localNow.wMonth) - (item.wYear * 12 + item.wMonth) == 1)
        group = 5;  // last month
    else if (item.wYear == localNow.wYear)
        group = 6;  // earlier this year
    else
        group = 7;  // a long time ago
    return kDateGroupFirst + group;
}

DWORD DayNumber(const SYSTEMTIME& localNow) { return (DWORD)DayIndex(localNow); }

size_t DateGroupValues::Write(const PROPERTYKEY& key) const {
    size_t written = 0;
    PROPVARIANT empty;
    PropVariantInit(&empty);
    ForEachItem(*this, [&](PCUITEMID_CHILD child, UINT group) {
        if (group) {
            PROPVARIANT value;
            InitPropVariantFromUInt32(group, &value);
            if (SUCCEEDED(SetViewValue(view, child, key, value))) written++;
        } else {
            SetViewValue(view, child, key, empty);
        }
    });
    return written;
}

bool DateGroupValues::Match(const PROPERTYKEY& key) const {
    bool match = true;
    ForEachItem(*this, [&](PCUITEMID_CHILD child, UINT group) {
        if (!match) return;
        PROPVARIANT value;
        PropVariantInit(&value);
        HRESULT hr = GetViewValue(view, child, key, &value);
        UINT actual = SUCCEEDED(hr) && value.vt == VT_UI4 ? value.ulVal : 0;
        PropVariantClear(&value);
        // An item that is not in the view (any more) has nothing to show.
        if (FAILED(hr) && hr != TYPE_E_ELEMENTNOTFOUND) return;
        if (actual != group) match = false;
    });
    return match;
}

}  // namespace ep
