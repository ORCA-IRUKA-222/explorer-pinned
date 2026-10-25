#include "displaycheck.h"

#include <uiautomation.h>

#include <algorithm>
#include <climits>
#include <map>

#include "resource.h"
#include "util.h"

namespace ep {
namespace {

struct Request {
    HWND window;
    std::wstring folder;
    std::wstring label;
    HWND notify;
    UINT message;
    LPARAM cookie;
};

// Group name -> topmost position and number of items Explorer has created elements for
// (those on screen and near it).
// `firstGroup` receives the topmost group, not counting list items outside any group (other
// lists of the window, or items read while Explorer was still drawing).
std::wstring DescribeGroups(HWND window, std::wstring* firstGroup) {
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))))
        return L"(UI Automation unavailable)";
    std::wstring result;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* cond = nullptr;
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
            if (firstGroup->empty() && o.second != L"(no group)") *firstGroup = o.second;
        }
        if (result.empty()) result = L" (no items)";
    } else {
        result = L" (not available)";
    }
    if (walker) walker->Release();
    if (items) items->Release();
    if (cond) cond->Release();
    if (root) root->Release();
    uia->Release();
    return result;
}

DWORD WINAPI CheckThread(void* param) {
    Request* request = static_cast<Request*>(param);
    Sleep(1500);  // let Explorer draw the new grouping
    DisplayResult result = DisplayResult::Unknown;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::wstring first;
        std::wstring groups = DescribeGroups(request->window, &first);
        // No group read (the list is still being drawn, or UI Automation failed): unknown.
        if (!first.empty()) result = first == request->label ? DisplayResult::Shown : DisplayResult::Missing;
        LogLine(L"display %s:%s%s", request->folder.c_str(), groups.c_str(),
                result == DisplayResult::Missing ? L"  <- the pinned group is not shown" : L"");
        CoUninitialize();
    }
    if (request->notify) PostMessageW(request->notify, request->message, (WPARAM)result, request->cookie);
    delete request;
    return 0;
}

}  // namespace

void CheckDisplayedGroups(HWND window, const std::wstring& folder, const std::wstring& pinnedLabel, HWND notify,
                          UINT message, LPARAM cookie) {
    Request* request = new Request{window, folder, pinnedLabel.empty() ? LoadStr(IDS_PROP_PINNED) : pinnedLabel,
                                   notify, message, cookie};
    HANDLE thread = CreateThread(nullptr, 0, CheckThread, request, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete request;
        if (notify) PostMessageW(notify, message, (WPARAM)DisplayResult::Unknown, cookie);
    }
}

}  // namespace ep
