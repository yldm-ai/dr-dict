#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl.h>
namespace dr
{
struct TrayMenuItem
{
    std::wstring text;    // empty: a separator
    wchar_t glyph = 0;    // Segoe Fluent Icons (or MDL2 Assets) code point drawn before the text; 0 for none
    std::function<void()> action;
};
// The colours the menu derives its surface, hover, separator and border tints from.
struct TrayPalette
{
    bool dark = false;
    UINT32 background = 0xffffff; // 0xRRGGBB
    UINT32 text = 0x37352f;
};

// A notification-area icon with its own context menu. The menu is a small Direct2D popup instead
// of a Win32 HMENU, so it follows the page's colour scheme: rounded by DWM on Windows 11 (with its
// system border and shadow), keyboard navigable, dismissed by Esc or a click elsewhere.
class Tray
{
  public:
    ~Tray();
    bool create(HINSTANCE instance, int icon_resource, const std::wstring &tip);
    /** Takes the icon out of the notification area now (it is also removed on destruction). */
    void remove();

    std::vector<TrayMenuItem> menu;
    std::function<TrayPalette()> palette; // read every time the menu opens
    std::function<void()> on_activate;    // left click or Enter/Space on the icon

  private:
    static LRESULT CALLBACK host_procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK menu_procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT host_message(UINT, WPARAM, LPARAM);
    LRESULT menu_message(UINT, WPARAM, LPARAM);
    bool add_icon();
    void show_menu(POINT anchor);
    void close_menu();
    void paint_menu();
    bool ensure_resources();
    int item_at(LPARAM client) const;
    void set_hot(int index);
    void step(int direction);
    void choose(int index);

    HINSTANCE instance_ = nullptr;
    HWND host_ = nullptr; // hidden top-level window: receives icon callbacks and TaskbarCreated
    HWND menu_window_ = nullptr;
    HICON icon_ = nullptr;
    std::wstring tip_;
    bool added_ = false;
    UINT taskbar_created_ = 0;

    // Menu state, in DIPs.
    float dpi_ = 96;
    float width_ = 0, height_ = 0;
    std::vector<float> tops_;
    std::vector<Microsoft::WRL::ComPtr<IDWriteTextLayout>> layouts_;
    int hot_ = -1, pressed_ = -1;
    bool tracking_ = false;
    bool system_border_ = false; // DWM draws the rounded border (Windows 11)
    TrayPalette colors_{};

    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> glyphs_;
};
} // namespace dr
