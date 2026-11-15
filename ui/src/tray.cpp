#include "dr/tray.h"
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
namespace dr
{
namespace
{
constexpr UINT icon_callback = WM_APP + 1;
constexpr UINT choose_message = WM_APP + 2;
constexpr UINT icon_id = 1;
// Layout in DIPs, after Notion's menus: tight outer padding, rows a little taller than the text,
// a quiet glyph column and hairline separators.
constexpr float padding = 5, row = 30, separator = 9, glyph_left = 10, glyph_box = 16, text_gap = 10,
                text_right = 28, min_width = 176, radius = 5;

D2D1_COLOR_F color(UINT32 rgb)
{
    return D2D1::ColorF(rgb);
}
// a + (b - a) * t per channel.
UINT32 mix(UINT32 a, UINT32 b, float t)
{
    auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xff), y = static_cast<float>((b >> shift) & 0xff);
        return static_cast<UINT32>(std::lround(x + (y - x) * t)) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}
struct Tints
{
    UINT32 surface, text, glyph, hover, pressed, rule, border;
};
// Every tint comes from the page's background and text, so sepia and coloured schemes keep their
// warmth. A dark menu is lifted slightly off the page so it reads as a surface above it.
Tints tints(const TrayPalette &p)
{
    const UINT32 surface = p.dark ? mix(p.background, 0xffffff, 0.06f) : p.background;
    return {surface,
            p.text,
            mix(surface, p.text, 0.68f),
            mix(surface, p.text, p.dark ? 0.09f : 0.06f),
            mix(surface, p.text, p.dark ? 0.14f : 0.10f),
            mix(surface, p.text, p.dark ? 0.12f : 0.09f),
            mix(surface, p.text, p.dark ? 0.18f : 0.13f)};
}
COLORREF colorref(UINT32 rgb)
{
    return RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
} // namespace

Tray::~Tray()
{
    remove();
    if (menu_window_)
        DestroyWindow(menu_window_);
    if (host_)
        DestroyWindow(host_);
    if (icon_)
        DestroyIcon(icon_);
}

bool Tray::create(HINSTANCE instance, int icon_resource, const std::wstring &tip)
{
    instance_ = instance;
    tip_ = tip;
    WNDCLASSEXW host{sizeof(host)};
    host.hInstance = instance;
    host.lpfnWndProc = host_procedure;
    host.lpszClassName = L"DRDict.TrayHost";
    RegisterClassExW(&host);
    WNDCLASSEXW popup{sizeof(popup)};
    popup.hInstance = instance;
    popup.lpfnWndProc = menu_procedure;
    popup.lpszClassName = L"DRDict.TrayMenu";
    popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // The system shadow of menus; Windows 11 rounds it with the window.
    popup.style = CS_DROPSHADOW;
    RegisterClassExW(&popup);
    // A hidden top-level window rather than a message-only one: TaskbarCreated is broadcast, and
    // message-only windows do not receive broadcasts.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, host.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, this);
    if (!host_)
        return false;
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(host_, taskbar_created_, MSGFLT_ALLOW, nullptr);
    const UINT dpi = GetDpiForSystem();
    icon_ = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(icon_resource), IMAGE_ICON,
                                          GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                          GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    return add_icon();
}

bool Tray::add_icon()
{
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = host_;
    data.uID = icon_id;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = icon_callback;
    data.hIcon = icon_;
    wcsncpy_s(data.szTip, tip_.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data);
    if (added_)
    {
        // Version 4 reports WM_CONTEXTMENU and NIN_SELECT with the anchor point in wParam.
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    return added_;
}

void Tray::remove()
{
    close_menu();
    if (!added_)
        return;
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = host_;
    data.uID = icon_id;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}

LRESULT CALLBACK Tray::host_procedure(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<Tray *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // host_ is set only once CreateWindowExW returns; creation messages go straight to the default.
    return self && self->host_ ? self->host_message(m, w, l) : DefWindowProcW(hwnd, m, w, l);
}

LRESULT Tray::host_message(UINT m, WPARAM w, LPARAM l)
{
    if (m == icon_callback)
    {
        switch (LOWORD(l))
        {
        case WM_CONTEXTMENU:
            show_menu({GET_X_LPARAM(w), GET_Y_LPARAM(w)});
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
            close_menu();
            if (on_activate)
                on_activate();
            break;
        }
        return 0;
    }
    if (m == choose_message)
    {
        // Run after the menu has gone, outside its window procedure: an action may quit.
        const auto index = static_cast<size_t>(w);
        if (index < menu.size() && menu[index].action)
            menu[index].action();
        return 0;
    }
    if (taskbar_created_ && m == taskbar_created_)
    {
        // Explorer restarted: the notification area starts empty.
        added_ = false;
        add_icon();
        return 0;
    }
    return DefWindowProcW(host_, m, w, l);
}

bool Tray::ensure_resources()
{
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
        return false;
    if (!write_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown **>(write_.GetAddressOf()))))
        return false;
    if (!text_)
    {
        // Segoe UI for Latin text; DirectWrite's zh-CN fallback sets Chinese in Microsoft YaHei UI.
        if (FAILED(write_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 13, L"zh-CN", &text_)))
            return false;
        text_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!glyphs_)
    {
        Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(write_->GetSystemFontCollection(&fonts)))
            for (const auto *name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"})
            {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists &&
                    SUCCEEDED(write_->CreateTextFormat(name, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14,
                                                       L"en-US", &glyphs_)))
                    break;
            }
        if (glyphs_)
        {
            glyphs_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            glyphs_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            glyphs_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    if (!menu_window_)
    {
        menu_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"DRDict.TrayMenu", L"DR Dict", WS_POPUP, 0,
                                       0, 0, 0, host_, nullptr, instance_, this);
        if (!menu_window_)
            return false;
        // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND. It fails before Windows 11, where the
        // menu stays square and draws its own hairline border.
        DWORD corners = 2;
        system_border_ = SUCCEEDED(DwmSetWindowAttribute(menu_window_, 33, &corners, sizeof(corners)));
    }
    return true;
}

void Tray::show_menu(POINT anchor)
{
    if (menu.empty() || !ensure_resources())
        return;
    colors_ = palette ? palette() : TrayPalette{};
    const auto tint = tints(colors_);
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpi_x,
                                &dpi_y)))
        dpi_x = GetDpiForSystem();
    dpi_ = static_cast<float>(dpi_x);

    // Rows and the width the longest label needs.
    const float text_left = padding + glyph_left + glyph_box + text_gap;
    float label = 0, y = padding;
    tops_.clear();
    layouts_.clear();
    for (const auto &item : menu)
    {
        tops_.push_back(y);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (!item.text.empty() && SUCCEEDED(write_->CreateTextLayout(item.text.c_str(),
                                                                       static_cast<UINT32>(item.text.size()),
                                                                       text_.Get(), 1000, row, &layout)))
        {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            label = std::max(label, metrics.widthIncludingTrailingWhitespace);
        }
        layouts_.push_back(layout);
        y += item.text.empty() ? separator : row;
    }
    width_ = std::max(min_width, std::ceil(text_left + label + text_right + padding));
    height_ = y + padding;
    for (auto &layout : layouts_)
        if (layout)
            layout->SetMaxWidth(width_ - text_left - padding);

    const auto pixels = [&](float dip) { return static_cast<LONG>(std::ceil(dip * dpi_ / 96)); };
    SIZE size{pixels(width_), pixels(height_)};
    RECT place{};
    // Opens up and to the right of the pointer like the shell's own tray menus, flipped to stay
    // inside the work area.
    if (!CalculatePopupWindowPosition(&anchor, &size, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_WORKAREA, nullptr,
                                      &place))
        place = {anchor.x, anchor.y - size.cy, anchor.x + size.cx, anchor.y};

    BOOL dark = colors_.dark;
    DwmSetWindowAttribute(menu_window_, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    if (system_border_)
    {
        COLORREF border = colorref(tint.border);
        DwmSetWindowAttribute(menu_window_, 34, &border, sizeof(border)); // DWMWA_BORDER_COLOR
    }
    hot_ = pressed_ = -1;
    SetWindowPos(menu_window_, HWND_TOPMOST, place.left, place.top, size.cx, size.cy, SWP_NOACTIVATE);
    if (target_)
        target_->Resize(D2D1::SizeU(size.cx, size.cy));
    // Painted before it is shown, so the first visible frame is complete.
    InvalidateRect(menu_window_, nullptr, FALSE);
    ShowWindow(menu_window_, SW_SHOW);
    UpdateWindow(menu_window_);
    // The notification area lets the process take the foreground while it handles the click;
    // without it, a click elsewhere would never deactivate (and dismiss) the menu.
    SetForegroundWindow(menu_window_);
    SetFocus(menu_window_);
}

void Tray::close_menu()
{
    if (!menu_window_ || !IsWindowVisible(menu_window_))
        return;
    ShowWindow(menu_window_, SW_HIDE);
    hot_ = pressed_ = -1;
    tracking_ = false;
    if (GetCapture() == menu_window_)
        ReleaseCapture();
    // As TrackPopupMenu's documentation asks of tray menus, so the next click is not swallowed.
    PostMessageW(host_, WM_NULL, 0, 0);
}

void Tray::paint_menu()
{
    PAINTSTRUCT ps{};
    BeginPaint(menu_window_, &ps);
    RECT rc{};
    GetClientRect(menu_window_, &rc);
    if (!target_ && factory_)
    {
        factory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(menu_window_, D2D1::SizeU(rc.right, rc.bottom)), &target_);
    }
    if (target_)
    {
        const auto tint = tints(colors_);
        target_->SetDpi(dpi_, dpi_);
        target_->BeginDraw();
        target_->Clear(color(tint.surface));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target_->CreateSolidColorBrush(color(tint.text), &brush);
        const float px = 96 / dpi_; // one device pixel in DIPs
        const float text_left = padding + glyph_left + glyph_box + text_gap;
        for (size_t i = 0; i < menu.size() && brush; ++i)
        {
            const auto &item = menu[i];
            const float top = tops_[i];
            if (item.text.empty())
            {
                // A hairline, snapped to the device pixel grid.
                const float line = std::floor((top + separator / 2) / px) * px;
                brush->SetColor(color(tint.rule));
                target_->FillRectangle(D2D1::RectF(0, line, width_, line + px), brush.Get());
                continue;
            }
            if (static_cast<int>(i) == hot_)
            {
                brush->SetColor(color(pressed_ == static_cast<int>(i) ? tint.pressed : tint.hover));
                target_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(padding, top, width_ - padding, top + row), radius, radius),
                    brush.Get());
            }
            if (item.glyph && glyphs_)
            {
                brush->SetColor(color(tint.glyph));
                const float x = padding + glyph_left;
                target_->DrawTextW(&item.glyph, 1, glyphs_.Get(), D2D1::RectF(x, top, x + glyph_box, top + row),
                                   brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            if (layouts_[i])
            {
                brush->SetColor(color(tint.text));
                target_->DrawTextLayout(D2D1::Point2F(text_left, top), layouts_[i].Get(), brush.Get(),
                                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (!system_border_ && brush)
        {
            brush->SetColor(color(tint.border));
            target_->DrawRectangle(D2D1::RectF(px / 2, px / 2, width_ - px / 2, height_ - px / 2), brush.Get(), px);
        }
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET)
            target_.Reset();
    }
    EndPaint(menu_window_, &ps);
}

int Tray::item_at(LPARAM l) const
{
    const float x = GET_X_LPARAM(l) * 96 / dpi_, y = GET_Y_LPARAM(l) * 96 / dpi_;
    if (x < padding || x >= width_ - padding)
        return -1;
    for (size_t i = 0; i < menu.size(); ++i)
        if (!menu[i].text.empty() && y >= tops_[i] && y < tops_[i] + row)
            return static_cast<int>(i);
    return -1;
}

void Tray::set_hot(int index)
{
    if (index == hot_)
        return;
    hot_ = index;
    InvalidateRect(menu_window_, nullptr, FALSE);
}

// Moves the highlight to the next (or previous) item, skipping separators and wrapping around.
void Tray::step(int direction)
{
    const int count = static_cast<int>(menu.size());
    int index = hot_;
    for (int tries = 0; tries < count; ++tries)
    {
        index = index < 0 ? (direction > 0 ? 0 : count - 1) : (index + direction + count) % count;
        if (!menu[index].text.empty())
        {
            set_hot(index);
            return;
        }
    }
}

void Tray::choose(int index)
{
    if (index < 0 || index >= static_cast<int>(menu.size()) || menu[index].text.empty())
        return;
    close_menu();
    PostMessageW(host_, choose_message, static_cast<WPARAM>(index), 0);
}

LRESULT CALLBACK Tray::menu_procedure(HWND hwnd, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<Tray *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->menu_window_)
        return DefWindowProcW(hwnd, m, w, l);
    return self->menu_message(m, w, l);
}

LRESULT Tray::menu_message(UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT:
        paint_menu();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            close_menu();
        break;
    case WM_MOUSEMOVE:
        if (!tracking_)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, menu_window_, 0};
            tracking_ = TrackMouseEvent(&track);
        }
        set_hot(item_at(l));
        return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        pressed_ = -1;
        set_hot(-1);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        pressed_ = item_at(l);
        set_hot(pressed_);
        InvalidateRect(menu_window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        const int index = item_at(l);
        const bool same = index >= 0 && index == pressed_;
        pressed_ = -1;
        InvalidateRect(menu_window_, nullptr, FALSE);
        if (same)
            choose(index);
        return 0;
    }
    case WM_KEYDOWN:
        switch (w)
        {
        case VK_DOWN:
        case VK_TAB:
            step(w == VK_TAB && GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            return 0;
        case VK_UP:
            step(-1);
            return 0;
        case VK_HOME:
            hot_ = -1;
            step(1);
            return 0;
        case VK_END:
            hot_ = -1;
            step(-1);
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            choose(hot_);
            return 0;
        case VK_ESCAPE:
            close_menu();
            return 0;
        }
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        close_menu();
        return 0;
    case WM_DESTROY:
        target_.Reset();
        return 0;
    }
    return DefWindowProcW(menu_window_, m, w, l);
}
} // namespace dr
