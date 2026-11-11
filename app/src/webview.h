#pragma once
#include "dr/paths.h"
#include <functional>
#include <objbase.h>
#include <windows.h>
#include <wrl.h>

// The generated WebView2 header requires COM definitions before its forward declarations.
#include <WebView2.h>

namespace dr
{
// Pages: https://app.dr-dict.local/ (bundled UI, folder mapping) and
// https://dict.dr-dict.local/<id>/… (articles and dictionary resources, answered natively).
inline constexpr wchar_t app_origin[] = L"https://app.dr-dict.local";
inline constexpr wchar_t app_index[] = L"https://app.dr-dict.local/index.html";
inline constexpr wchar_t app_popup[] = L"https://app.dr-dict.local/popup.html";
inline constexpr wchar_t dict_origin[] = L"https://dict.dr-dict.local/";

struct ResourceReply
{
    int status = 404;
    std::string mime = "text/plain";
    std::string body;
};

class WebView
{
  public:
    // `page` is the only document the view may show (and accept messages from). With `drag_regions`,
    // CSS app-region: drag areas move the host window.
    void create(HWND window, const fs::path &web_root, const fs::path &data_root, const wchar_t *page = app_index,
                bool drag_regions = false);
    void resize(RECT bounds);
    /** Tells the controller its host window moved or changed z-band (keeps input and popups aligned). */
    void notify_moved();
    /** Whether the view renders; a hidden host's view is told so it repaints when shown again. */
    void set_visible(bool visible);
    /** Gives keyboard focus to the page (an activated host window does not pass it on by itself). */
    void focus();
    bool ready() const { return ready_; }
    std::function<void()> on_ready; // the page finished its first navigation
    void send(const Json &message);
    // Page zoom as in a browser (Ctrl+wheel, Ctrl +/−/0), applied to the whole page and its frames.
    void set_zoom(double factor);
    double zoom() const { return zoom_; }
    std::function<void(double)> on_zoom; // after a change once the view exists (not the initial value)
    std::function<void(const Json &)> on_message;
    std::function<void(const std::wstring &)> on_error;
    std::function<void()> on_toggle_borderless;
    // Called on the UI thread; `reply` must also be invoked on the UI thread (once).
    std::function<void(const std::wstring &uri, std::function<void(ResourceReply)> reply)> on_resource;

  private:
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> view_;
    RECT bounds_{};
    double zoom_ = 1.0;
    std::wstring page_ = app_index;
    bool drag_regions_ = false;
    bool ready_ = false;
    void fail(HRESULT error, const wchar_t *step);
    void configure(const fs::path &web_root);
};
} // namespace dr
