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
    void create(HWND window, const fs::path &web_root, const fs::path &data_root);
    void resize(RECT bounds);
    void send(const Json &message);
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
    void fail(HRESULT error, const wchar_t *step);
    void configure(const fs::path &web_root);
};
} // namespace dr
