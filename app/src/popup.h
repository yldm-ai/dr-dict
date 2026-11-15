#pragma once
#include <functional>
#include <string_view>
#include <windows.h>
namespace dr
{
// Other programs (DR Reader's “查词典”) look a word up in the popup by finding its host window by
// class and sending it WM_COPYDATA with dwData = lookup_request and UTF-8 JSON
// {"word": "...", "x": screen x, "y": screen y} — the point the popup opens beside. The sender
// should AllowSetForegroundWindow DR Dict's process first, so the popup may take focus.
inline constexpr wchar_t popup_class[] = L"DrDictPopup";
inline constexpr ULONG_PTR lookup_request = 0x4B4C5244; // "DRLK"
// DR Dict started again with the same data folder: {"files": ["utf-8 path", ...]}. The running
// instance comes to the front and imports the files, and the new process exits (see main.cpp).
inline constexpr ULONG_PTR open_request = 0x4E504F44; // "DOPN"
// A window property on the popup host naming its instance's data folder (instance_tag), so a
// request reaches the instance that owns the same history and settings.
inline constexpr wchar_t instance_property[] = L"DRDict.Instance";

// The Ctrl+C+C lookup popup's host window: borderless, resizable from a thin edge around the
// WebView, shown next to the mouse pointer and kept above other windows.
//
// Behaviour after GoldenDict-ng's ScanPopup (src/ui/scanpopup.cc), implemented independently:
// it opens beside the pointer (to the right and below, flipped when there is no room); while
// unpinned it hides when the pointer moves away before ever entering it, when the pointer leaves
// it for a moment, when another window is activated, or on Esc. A pinned popup stays put.
//
// TOPMOST and WebView2 (from MSIME-Windows, windows_webview2.cpp): in a uiAccess process a host
// that is already TOPMOST makes CreateCoreWebView2Controller fail with E_INVALIDARG (UIPI blocks
// WebView2's cross-process SetParent, WebView2Feedback #486). So the host is created without
// WS_EX_TOPMOST and pinned only once its WebView is up (make_topmost), and every band change or
// move is followed by on_moved so the controller re-reads its parent's position.
class Popup
{
  public:
    ~Popup();
    bool create(HINSTANCE instance, HWND owner);
    HWND handle() const { return hwnd_; }

    /** Shows the popup beside `cursor` (screen pixels), or where it is if it is already shown. */
    void show_near(POINT cursor);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    void set_pinned(bool pinned);
    bool pinned() const { return pinned_; }
    /** Pins the host above other windows; call once its WebView has painted (see above). */
    void make_topmost();
    void schedule_topmost(UINT delay_ms);
    bool topmost() const { return topmost_; }
    /** Edge colour and dark frame to match the page ("#rrggbb" as 0xRRGGBB). */
    void set_colors(UINT32 background, bool dark);
    /** Size in DIPs used for the next show. */
    void set_size(int width, int height);
    SIZE size() const { return {width_, height_}; }

    std::function<void(RECT)> on_bounds; // WebView bounds inside the client area
    std::function<void()> on_moved;      // after a move, resize or z-band change
    std::function<void()> on_shown;
    std::function<void()> on_focus; // the host got keyboard focus; hand it to the WebView
    std::function<void()> on_hidden;
    std::function<void(int width, int height)> on_resized; // DIPs, after the user resizes
    std::function<void()> on_topmost;                      // the host has been pinned above other windows
    std::function<void(std::string_view)> on_request;      // a lookup_request's payload (see above)
    std::function<void(std::string_view)> on_open;         // an open_request's payload (see above)

  private:
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    int scale(int value) const;
    void layout();
    void track_pointer();

    HWND hwnd_ = nullptr;
    HWND previous_ = nullptr; // the window that had focus before the popup took it
    HBRUSH brush_ = nullptr;
    bool pinned_ = false;
    bool topmost_ = false;
    int width_ = 360, height_ = 420;
    // Pointer tracking while unpinned.
    ULONGLONG shown_at_ = 0;
    bool entered_ = false;
    ULONGLONG outside_since_ = 0;
};
} // namespace dr
