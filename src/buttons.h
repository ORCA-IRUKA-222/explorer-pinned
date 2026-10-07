#pragma once

#include "common.h"

#include <map>
#include <vector>

namespace ep {

// The on/off buttons in Explorer windows. Each button is owned by its Explorer window, so it
// stays above that window and hides with it. An owned window shares its input with the
// window's, so the buttons run on a thread of their own that never waits for anything (the
// agent's thread waits for Explorer in every call it makes).
class ExplorerButtons {
public:
    struct Tab {
        HWND view;  // the tab's folder view (SHELLDLL_DefView)
        bool pins;  // its folder has pinned items
        bool operator==(const Tab& o) const { return view == o.view && pins == o.pins; }
    };
    using Frames = std::map<HWND, std::vector<Tab>>;  // Explorer windows and their tabs

    ExplorerButtons();
    ~ExplorerButtons();
    ExplorerButtons(const ExplorerButtons&) = delete;
    ExplorerButtons& operator=(const ExplorerButtons&) = delete;

    void Start();
    void Stop();
    // The windows to show a button in, whether pins are on, and whether buttons are wanted.
    void Update(const Frames& frames, bool on, bool show);

    struct Thread;  // the button thread (buttons.cpp)

private:
    Thread* thread_ = nullptr;
    Frames sent_;
    bool sentOn_ = true;
    bool sentShow_ = true;
    bool sentOnce_ = false;
};

}  // namespace ep
