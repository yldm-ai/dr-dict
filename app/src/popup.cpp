#include "popup.h"
#include <algorithm>
#include <dwmapi.h>
#include <shellscalingapi.h>
namespace dr
{
namespace
{
constexpr UINT_PTR track_timer = 1;
constexpr UINT_PTR topmost_timer = 2;
constexpr UINT track_interval_ms = 30;
// GoldenDict-ng's popup gives a pointer that never entered it a 60 px grace zone and one that
// has been inside 400 ms to come back; the same distances read naturally here.
constexpr int grace_distance = 60;
constexpr ULONGLONG leave_delay_ms = 400;
constexpr int resize_edge = 5; // DIPs of host window around the WebView that resize the popup

COLORREF colorref(UINT32 rgb)
{
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
bool button_down()
{
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}
// A background process may not take the foreground; joining the foreground thread's input
// state for the call is the usual way around it for a window the user just summoned. One call
// only: a failed attempt followed by a second one bounces activation (active, inactive, active),
// and the inactive step would close the popup.
void bring_to_foreground(HWND hwnd)
{
    const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD self = GetCurrentThreadId();
    const bool attached = foreground && foreground != self && AttachThreadInput(self, foreground, TRUE);
    SetForegroundWindow(hwnd);
    if (attached)
        AttachThreadInput(self, foreground, FALSE);
}
} // namespace

Popup::~Popup()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
    if (brush_)
        DeleteObject(brush_);
}

bool Popup::create(HINSTANCE instance, HWND /*owner*/)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = popup_class;
    RegisterClassExW(&wc);
    brush_ = CreateSolidBrush(RGB(255, 255, 255));
    // Unowned, so it still shows while the main window is minimised; a tool window, so it has
    // no taskbar button. Not TOPMOST yet (see the class comment).
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, popup_class, L"DR Dict", WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                            0, 0, 360, 420, nullptr, nullptr, instance, this);
    if (!hwnd_)
        return false;
    // UIPI drops WM_COPYDATA from ordinary programs to a uiAccess one; lookup requests are let in.
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    // Rounded corners and the system shadow, as for other Windows 11 flyouts.
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

int Popup::scale(int value) const
{
    return MulDiv(value, hwnd_ ? GetDpiForWindow(hwnd_) : 96, 96);
}

void Popup::layout()
{
    RECT client{};
    GetClientRect(hwnd_, &client);
    InflateRect(&client, -scale(resize_edge), -scale(resize_edge));
    if (on_bounds)
        on_bounds(client);
}

void Popup::set_size(int width, int height)
{
    width_ = width;
    height_ = height;
}

void Popup::set_colors(UINT32 background, bool dark)
{
    if (brush_)
        DeleteObject(brush_);
    brush_ = CreateSolidBrush(colorref(background));
    BOOL value = dark;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void Popup::show_near(POINT cursor)
{
    const bool shown = visible();
    if (!shown || !pinned_)
    {
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        const RECT work = info.rcWork;
        UINT dpi_x = 96, dpi_y = 96;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
        const int w = std::min<int>(MulDiv(width_, dpi_x, 96), work.right - work.left);
        const int h = std::min<int>(MulDiv(height_, dpi_x, 96), work.bottom - work.top);
        const int gap_x = MulDiv(4, dpi_x, 96), gap_y = MulDiv(15, dpi_x, 96);
        // Right of the pointer, else left of it, else centred; below it, else above, else centred.
        int x = cursor.x + gap_x;
        if (x + w > work.right)
            x = cursor.x - gap_x - w >= work.left ? cursor.x - gap_x - w : work.left + (work.right - work.left - w) / 2;
        int y = cursor.y + gap_y;
        if (y + h > work.bottom)
            y = cursor.y - gap_y - h >= work.top ? cursor.y - gap_y - h : work.top + (work.bottom - work.top - h) / 2;
        SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    else
        SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (!shown)
        previous_ = GetForegroundWindow();
    shown_at_ = GetTickCount64();
    // Focus, so Esc and typing go to the popup; hide() hands it back.
    bring_to_foreground(hwnd_);
    if (!shown && on_shown)
        on_shown();
    // Already active (a pinned popup asked again): no WM_SETFOCUS will come, so pass focus now.
    if (GetForegroundWindow() == hwnd_ && on_focus)
        on_focus();
    if (on_moved)
        on_moved();
    entered_ = false;
    outside_since_ = 0;
    if (!pinned_)
        SetTimer(hwnd_, track_timer, track_interval_ms, nullptr);
}

void Popup::hide()
{
    if (!visible())
        return;
    KillTimer(hwnd_, track_timer);
    const bool focused = GetForegroundWindow() == hwnd_;
    ShowWindow(hwnd_, SW_HIDE);
    // Give focus back to where the user copied from, unless they already moved on.
    if (focused && previous_ && IsWindow(previous_))
        SetForegroundWindow(previous_);
    previous_ = nullptr;
    if (on_hidden)
        on_hidden();
}

void Popup::set_pinned(bool pinned)
{
    pinned_ = pinned;
    if (pinned)
        KillTimer(hwnd_, track_timer);
    else if (visible())
    {
        // Unpinning a popup the pointer is over: it hides once the pointer leaves.
        entered_ = true;
        outside_since_ = 0;
        SetTimer(hwnd_, track_timer, track_interval_ms, nullptr);
    }
}

void Popup::make_topmost()
{
    if (!hwnd_)
        return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    topmost_ = true;
    if (on_moved)
        on_moved();
    if (on_topmost)
        on_topmost();
}

void Popup::schedule_topmost(UINT delay_ms)
{
    if (hwnd_ && !topmost_)
        SetTimer(hwnd_, topmost_timer, delay_ms, nullptr);
}

void Popup::track_pointer()
{
    if (pinned_ || !visible())
    {
        KillTimer(hwnd_, track_timer);
        return;
    }
    // A pressed button means a drag, resize or text selection is under way.
    if (button_down())
        return;
    POINT p{};
    GetCursorPos(&p);
    RECT r{};
    GetWindowRect(hwnd_, &r);
    if (PtInRect(&r, p))
    {
        entered_ = true;
        outside_since_ = 0;
        return;
    }
    if (!entered_)
    {
        // Never entered: hide as soon as the pointer is clearly heading elsewhere.
        InflateRect(&r, scale(grace_distance), scale(grace_distance));
        if (!PtInRect(&r, p))
            hide();
        return;
    }
    const auto now = GetTickCount64();
    if (!outside_since_)
        outside_since_ = now;
    else if (now - outside_since_ >= leave_delay_ms)
        hide();
}

LRESULT CALLBACK Popup::procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_NCCREATE)
    {
        auto *self = static_cast<Popup *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto *self = reinterpret_cast<Popup *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(message, w, l) : DefWindowProcW(hwnd, message, w, l);
}

LRESULT Popup::handle(UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case WM_NCCALCSIZE:
        if (w)
            return 0; // the whole window is client area; WS_THICKFRAME only keeps resizing and the shadow
        break;
    case WM_NCHITTEST: {
        // Only the thin edge around the WebView reaches here; it resizes the popup.
        POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        RECT r{};
        GetWindowRect(hwnd_, &r);
        const int e = scale(resize_edge) + 1;
        const bool left = p.x < r.left + e, right = p.x >= r.right - e, top = p.y < r.top + e, bottom = p.y >= r.bottom - e;
        if (top && left)
            return HTTOPLEFT;
        if (top && right)
            return HTTOPRIGHT;
        if (bottom && left)
            return HTBOTTOMLEFT;
        if (bottom && right)
            return HTBOTTOMRIGHT;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
        if (top)
            return HTTOP;
        if (bottom)
            return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // The edge around the WebView, in the page's background colour.
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        FillRect(dc, &ps.rcPaint, brush_);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        layout();
        break;
    case WM_MOVE:
        if (on_moved)
            on_moved();
        break;
    case WM_EXITSIZEMOVE: {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        const int dpi = GetDpiForWindow(hwnd_);
        width_ = MulDiv(r.right - r.left, 96, dpi);
        height_ = MulDiv(r.bottom - r.top, 96, dpi);
        if (on_resized)
            on_resized(width_, height_);
        if (on_moved)
            on_moved();
        break;
    }
    case WM_DPICHANGED: {
        const auto *suggested = reinterpret_cast<const RECT *>(l);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ACTIVATE:
        // Another window was activated (a click elsewhere): an unpinned popup goes away.
        // Activation can still settle for a moment after show_near takes the foreground.
        if (LOWORD(w) == WA_INACTIVE && !pinned_ && visible() && GetTickCount64() - shown_at_ > 300)
        {
            previous_ = nullptr;
            hide();
        }
        break;
    case WM_TIMER:
        if (w == track_timer)
        {
            track_pointer();
            return 0;
        }
        if (w == topmost_timer)
        {
            KillTimer(hwnd_, topmost_timer);
            make_topmost();
            return 0;
        }
        break;
    case WM_SETFOCUS:
        // Keyboard input belongs to the page (Esc, typing into its search box).
        if (on_focus)
            on_focus();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            hide();
            return 0;
        }
        break;
    case WM_CLOSE:
        hide();
        return 0;
    case WM_COPYDATA: {
        // A lookup from another program; the payload is only valid during this message.
        const auto *data = reinterpret_cast<const COPYDATASTRUCT *>(l);
        if (!data || !data->lpData || data->cbData > 1024 * 1024)
            return FALSE;
        const std::string_view payload(static_cast<const char *>(data->lpData), data->cbData);
        if (data->dwData == lookup_request && on_request && payload.size() <= 64 * 1024)
            on_request(payload);
        else if (data->dwData == open_request && on_open)
            on_open(payload);
        else
            return FALSE;
        return TRUE;
    }
    }
    return DefWindowProcW(hwnd_, message, w, l);
}
} // namespace dr
