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
};

// Group name -> topmost position and number of items Explorer has created elements for
// (those on screen and near it).
std::wstring DescribeGroups(HWND window) {
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
        for (const auto& o : order)
            result += L" [" + o.second + L"] " + std::to_wstring(groups[o.second].count);
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
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::wstring groups = DescribeGroups(request->window);
        std::wstring expected = L"[" + LoadStr(IDS_PROP_PINNED) + L"]";
        bool shown = groups.compare(0, expected.size() + 1, L" " + expected) == 0;
        LogLine(L"display %s:%s%s", request->folder.c_str(), groups.c_str(),
                shown ? L"" : L"  <- the pinned group is not shown");
        CoUninitialize();
    }
    delete request;
    return 0;
}

}  // namespace

void LogDisplayedGroups(HWND window, const std::wstring& folder) {
    Request* request = new Request{window, folder};
    HANDLE thread = CreateThread(nullptr, 0, CheckThread, request, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete request;
}

}  // namespace ep
