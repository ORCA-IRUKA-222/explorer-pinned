#include "toggle.h"

#include <algorithm>
#include <cmath>
// gdiplus.h uses min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "util.h"

namespace ep {
namespace {

bool ReadFlag(const wchar_t* name) {
    std::wstring v;
    return !(RegReadString(HKEY_CURRENT_USER, kRegRoot, name, &v) && v == L"0");
}

bool Japanese() { return PRIMARYLANGID(UiLanguage()) == LANG_JAPANESE; }

const wchar_t* Label(bool on) {
    if (Japanese()) return on ? L"ピン止め オン" : L"ピン止め オフ";
    return on ? L"Pins on" : L"Pins off";
}

// Window text, read by screen readers.
const wchar_t* Description(bool on) {
    if (Japanese()) return on ? L"ピン止め表示: オン（クリックでオフ）" : L"ピン止め表示: オフ（クリックでオン）";
    return on ? L"Pinned group: on (click to turn off)" : L"Pinned group: off (click to turn on)";
}

bool DarkTheme() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

void StartGdiplus() {
    static ULONG_PTR token = 0;
    if (!token) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
}

HINSTANCE ThisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &m);
    return m;
}

HWND AncestorOfClass(HWND hwnd, const wchar_t* cls) {
    for (HWND p = GetParent(hwnd); p; p = GetParent(p)) {
        wchar_t name[64];
        if (GetClassNameW(p, name, ARRAYSIZE(name)) && wcscmp(name, cls) == 0) return p;
    }
    return nullptr;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

// A push pin in an s x s box, tilted like the one in the program's icon.
void DrawPin(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, float x, float y, float s) {
    Gdiplus::GraphicsState state = g.Save();
    g.TranslateTransform(x + s / 2, y + s / 2);
    g.RotateTransform(40);
    g.TranslateTransform(-s / 2, -s / 2);
    Gdiplus::GraphicsPath head;
    AddRoundRect(head, 0.30f * s, 0.08f * s, 0.40f * s, 0.14f * s, 0.05f * s);
    g.FillPath(&brush, &head);
    Gdiplus::PointF body[4] = {{0.37f * s, 0.20f * s}, {0.63f * s, 0.20f * s}, {0.66f * s, 0.50f * s}, {0.34f * s, 0.50f * s}};
    g.FillPolygon(&brush, body, 4);
    Gdiplus::GraphicsPath collar;
    AddRoundRect(collar, 0.20f * s, 0.48f * s, 0.60f * s, 0.11f * s, 0.04f * s);
    g.FillPath(&brush, &collar);
    Gdiplus::PointF needle[3] = {{0.46f * s, 0.57f * s}, {0.54f * s, 0.57f * s}, {0.50f * s, 0.96f * s}};
    g.FillPolygon(&brush, needle, 3);
    g.Restore(state);
}

}  // namespace

bool PinsEnabled() { return ReadFlag(L"Enabled"); }
void SetPinsEnabled(bool on) { RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Enabled", on ? L"1" : L"0"); }
bool ToggleButtonEnabled() { return ReadFlag(L"Button"); }
void SetToggleButtonEnabled(bool on) { RegWriteString(HKEY_CURRENT_USER, kRegRoot, L"Button", on ? L"1" : L"0"); }

void ToggleButton::Show(HWND owner, HWND folderView, bool on) {
    if (hwnd_ && (owner_ != owner || !IsWindow(hwnd_))) Destroy();
    bool repaint = !hwnd_ || on != on_;
    owner_ = owner;
    view_ = folderView;
    on_ = on;
    UINT dpi = GetDpiForWindow(folderView);
    if (dpi && dpi != dpi_) {
        dpi_ = dpi;
        repaint = true;
    }
    if (!hwnd_) {
        HINSTANCE inst = ThisModule();
        WNDCLASSEXW wc = {sizeof(wc)};
        if (!GetClassInfoExW(inst, kToggleClass, &wc)) {
            wc = {sizeof(wc)};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
            wc.lpszClassName = kToggleClass;
            RegisterClassExW(&wc);
        }
        // Owned by the window: stays above it, and hides and goes away with it.
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kToggleClass, L"", WS_POPUP, 0, 0,
                                1, 1, owner, nullptr, inst, this);
        if (!hwnd_) return;
    }
    if (repaint) {
        SetWindowTextW(hwnd_, Description(on));
        Paint();
    }
    Place();
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void ToggleButton::Place() {
    if (!hwnd_ || !IsWindow(view_)) return;
    UINT dpi = GetDpiForWindow(view_);
    if (dpi && dpi != dpi_) {  // moved to a monitor with another scale
        dpi_ = dpi;
        Paint();
    }
    RECT view;
    GetWindowRect(view_, &view);
    float k = dpi_ / 96.0f;
    int x, y;
    // Explorer draws its status bar below the folder view, inside the same DirectUI host.
    HWND host = AncestorOfClass(view_, L"DUIViewWndClassName");
    RECT hostRect = {};
    int bar = host && GetWindowRect(host, &hostRect) ? hostRect.bottom - view.bottom : 0;
    if (bar >= (int)(18 * k) && bar <= (int)(48 * k)) {
        // Right part of the status bar, left of the view buttons.
        x = hostRect.right - size_.cx - (int)(96 * k);
        y = view.bottom + (bar - size_.cy) / 2;
    } else {
        // Bottom-right corner of the folder view, clear of the scroll bar.
        x = view.right - size_.cx - (int)(26 * k);
        y = view.bottom - size_.cy - (int)(10 * k);
    }
    RECT now;
    GetWindowRect(hwnd_, &now);
    if (now.left != x || now.top != y || now.right - now.left != size_.cx || now.bottom - now.top != size_.cy)
        SetWindowPos(hwnd_, nullptr, x, y, size_.cx, size_.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void ToggleButton::Hide() {
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

void ToggleButton::Destroy() {
    if (!hwnd_) return;
    HWND hwnd = hwnd_;
    hwnd_ = nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
}

void ToggleButton::Paint() {
    StartGdiplus();
    float k = dpi_ / 96.0f;
    const wchar_t* label = Label(on_);
    Gdiplus::FontFamily preferred(Japanese() ? L"Meiryo UI" : L"Segoe UI");
    const Gdiplus::FontFamily* family = preferred.IsAvailable() ? &preferred : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font font(family, 12.0f * k, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    const Gdiplus::StringFormat* format = Gdiplus::StringFormat::GenericTypographic();

    HDC screen = GetDC(nullptr);
    Gdiplus::RectF text;
    {
        Gdiplus::Graphics measure(screen);
        measure.MeasureString(label, -1, &font, Gdiplus::PointF(0, 0), format, &text);
    }
    int h = (int)std::ceil(24 * k);
    float pad = 9 * k, pin = 13 * k, gap = 5 * k;
    int w = (int)std::ceil(pad + pin + gap + text.Width + pad);
    size_ = {w, h};

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        bool dark = DarkTheme();
        Gdiplus::Color fill, border, ink;
        if (on_) {
            fill = hover_ ? Gdiplus::Color(255, 232, 118, 14) : Gdiplus::Color(255, 255, 138, 31);
            border = fill;
            ink = Gdiplus::Color(255, 255, 255, 255);
        } else if (dark) {
            fill = hover_ ? Gdiplus::Color(255, 72, 78, 88) : Gdiplus::Color(255, 58, 63, 71);
            border = Gdiplus::Color(255, 92, 99, 110);
            ink = Gdiplus::Color(255, 214, 220, 227);
        } else {
            fill = hover_ ? Gdiplus::Color(255, 226, 232, 239) : Gdiplus::Color(255, 240, 243, 247);
            border = Gdiplus::Color(255, 196, 206, 218);
            ink = Gdiplus::Color(255, 74, 88, 104);
        }
        Gdiplus::GraphicsPath pill;
        AddRoundRect(pill, 0.5f, 0.5f, w - 1.0f, h - 1.0f, (h - 1.0f) / 2);
        Gdiplus::SolidBrush fillBrush(fill);
        g.FillPath(&fillBrush, &pill);
        Gdiplus::Pen pen(border, std::max(1.0f, k));
        g.DrawPath(&pen, &pill);
        Gdiplus::SolidBrush inkBrush(ink);
        DrawPin(g, inkBrush, pad, (h - pin) / 2, pin);
        g.DrawString(label, -1, &font, Gdiplus::PointF(pad + pin + gap, (h - text.Height) / 2), format, &inkBrush);
    }
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size_, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK ToggleButton::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<ToggleButton*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // the Explorer window or dialog keeps the focus
        case WM_MOUSEMOVE:
            if (self && !self->hover_) {
                self->hover_ = true;
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                self->Paint();
            }
            return 0;
        case WM_MOUSELEAVE:
            if (self) {
                self->hover_ = false;
                self->Paint();
            }
            return 0;
        case WM_NCDESTROY:
            // Also when Windows destroys it together with its window.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (self && self->hwnd_ == hwnd) self->hwnd_ = nullptr;
            break;
        case WM_LBUTTONUP:
            if (self && self->onClick) {
                auto click = self->onClick;  // the handler may destroy this button
                click();
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ep
