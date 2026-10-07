#pragma once

#include "common.h"

#include <functional>

namespace ep {

// One switch for everything: when off, Explorer windows and file dialogs show their usual
// grouping (pins are kept). HKCU\Software\ExplorerPinned: Enabled = "0" turns it off.
bool PinsEnabled();
void SetPinsEnabled(bool on);
// The on/off button in Explorer windows and file dialogs (Button = "0" hides it).
bool ToggleButtonEnabled();
void SetToggleButtonEnabled(bool on);

inline constexpr wchar_t kToggleClass[] = L"ExplorerPinned.Toggle";

// A small "Pins: On / Off" button over a folder view: an owned popup window (so it stays with
// its window), placed in the status bar below the view or else in the view's bottom-right
// corner. Lives on the thread that created it.
class ToggleButton {
public:
    ToggleButton() = default;
    ToggleButton(const ToggleButton&) = delete;
    ToggleButton& operator=(const ToggleButton&) = delete;
    ~ToggleButton() { Destroy(); }

    // Shows the button for `owner` (a top-level window) next to `folderView` (SHELLDLL_DefView).
    void Show(HWND owner, HWND folderView, bool on);
    // Moves it after the window moved or was resized.
    void Place();
    void Hide();
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

    std::function<void()> onClick;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Paint();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND view_ = nullptr;
    bool on_ = true;
    bool hover_ = false;
    UINT dpi_ = 96;
    SIZE size_ = {};
};

}  // namespace ep
