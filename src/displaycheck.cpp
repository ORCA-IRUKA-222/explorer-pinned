#include "displaycheck.h"

#include <propsys.h>
#include <propvarutil.h>
#include <uiautomation.h>

#include <algorithm>
#include <climits>
#include <map>

#include "resource.h"
#include "util.h"

namespace ep {
namespace {

// The pinned group's name as Explorer shows it: the label of the registered property (it is
// in the language of the setup), or the program's own.
bool IsPinnedLabel(const std::wstring& name) {
    if (name == LoadStr(IDS_PROP_PINNED)) return true;
    IPropertyDescription* desc = nullptr;
    if (FAILED(PSGetPropertyDescription(kPinStateKeys[0], IID_PPV_ARGS(&desc)))) return false;
    PROPVARIANT value;
    InitPropVariantFromUInt32(kPinnedValue, &value);
    PWSTR text = nullptr;
    bool same = SUCCEEDED(desc->FormatForDisplay(value, PDFF_DEFAULT, &text)) && text && name == text;
    CoTaskMemFree(text);
    desc->Release();
    return same;
}

// The groups Explorer has created elements for (those on screen and near it), top to
// bottom, with their number of items: " [Pinned] 2 [Unspecified] 14". `first` receives the
// topmost group, not counting list items outside any group (other lists of the window, or
// items read while Explorer was still drawing). `scroll` receives the vertical scroll
// position of the list in percent (UIA_ScrollPatternNoScroll when it does not scroll;
// below that when there is no list).
std::wstring DescribeGroups(IUIAutomation* uia, HWND window, std::wstring* first, double* scroll) {
    *scroll = -2;
    std::wstring result;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* cond = nullptr;
    IUIAutomationCondition* scrollCond = nullptr;
    IUIAutomationElementArray* items = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    VARIANT type;
    type.vt = VT_I4;
    type.lVal = UIA_ListItemControlTypeId;
    if (SUCCEEDED(uia->ElementFromHandle(window, &root)) && root &&
        SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &cond)) &&
        SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &items)) && items &&
        SUCCEEDED(uia->get_ControlViewWalker(&walker))) {
        struct Group {
            int top = INT_MAX;
            int count = 0;
        };
        std::map<std::wstring, Group> groups;
        int length = 0;
        items->get_Length(&length);
        for (int i = 0; i < length; i++) {
            IUIAutomationElement* item = nullptr;
            if (FAILED(items->GetElement(i, &item)) || !item) continue;
            IUIAutomationElement* parent = nullptr;
            std::wstring name = L"(no group)";
            if (SUCCEEDED(walker->GetParentElement(item, &parent)) && parent) {
                CONTROLTYPEID parentType = 0;
                BSTR text = nullptr;
                parent->get_CurrentControlType(&parentType);
                if (parentType == UIA_GroupControlTypeId && SUCCEEDED(parent->get_CurrentName(&text)) && text) name = text;
                SysFreeString(text);
                parent->Release();
            }
            RECT r = {};
            item->get_CurrentBoundingRectangle(&r);
            Group& g = groups[name];
            g.top = std::min(g.top, (int)r.top);
            g.count++;
            item->Release();
        }
        std::vector<std::pair<int, std::wstring>> order;
        for (const auto& g : groups) order.push_back({g.second.top, g.first});
        std::sort(order.begin(), order.end());
        for (const auto& o : order) {
            result += L" [" + o.second + L"] " + std::to_wstring(groups[o.second].count);
            if (first->empty() && o.second != L"(no group)") *first = o.second;
        }
        if (result.empty()) result = L" (no items)";

        VARIANT yes;
        yes.vt = VT_BOOL;
        yes.boolVal = VARIANT_TRUE;
        IUIAutomationElement* list = nullptr;
        IUIAutomationScrollPattern* pattern = nullptr;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, yes, &scrollCond)) &&
            SUCCEEDED(root->FindFirst(TreeScope_Subtree, scrollCond, &list)) && list &&
            SUCCEEDED(list->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&pattern))) && pattern)
            pattern->get_CurrentVerticalScrollPercent(scroll);
        if (pattern) pattern->Release();
        if (list) list->Release();
    } else {
        result = L" (not available)";
    }
    if (walker) walker->Release();
    if (items) items->Release();
    if (scrollCond) scrollCond->Release();
    if (cond) cond->Release();
    if (root) root->Release();
    return result;
}

DWORD WINAPI CheckThread(void* param) {
    DisplayCheck* check = static_cast<DisplayCheck*>(param);
    Sleep(check->delayMs);
    Display state = Display::Unknown;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        IUIAutomation* uia = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
            std::wstring first;
            double scroll = -2;
            std::wstring groups = DescribeGroups(uia, check->window, &first, &scroll);
            // Only the top of the list tells which group comes first.
            bool top = scroll >= UIA_ScrollPatternNoScroll && scroll <= 0;
            const wchar_t* note = L"";
            if (first.empty()) {
                note = L"  (no groups)";
            } else if (IsPinnedLabel(first)) {
                state = Display::Shown;
            } else if (top) {
                state = Display::NotShown;
                note = L"  <- the pinned group is not shown";
            } else {
                note = L"  (scrolled down)";
            }
            LogLine(L"display %s:%s%s", check->folder.c_str(), groups.c_str(), note);
            uia->Release();
        }
        CoUninitialize();
    }
    if (check->notify) PostMessageW(check->notify, check->message, check->cookie, (LPARAM)state);
    delete check;
    return 0;
}

}  // namespace

void CheckDisplayedGroups(const DisplayCheck& check) {
    DisplayCheck* copy = new DisplayCheck(check);
    HANDLE thread = CreateThread(nullptr, 0, CheckThread, copy, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete copy;
}

}  // namespace ep
