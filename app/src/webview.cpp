#include "webview.h"
#include "dr/text.h"
#include <shlwapi.h>
#include <sstream>
namespace dr
{
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace
{
std::wstring take(LPWSTR value)
{
    std::wstring result = value ? value : L"";
    CoTaskMemFree(value);
    return result;
}
const wchar_t *reason(int status)
{
    switch (status)
    {
    case 200: return L"OK";
    case 400: return L"Bad Request";
    case 404: return L"Not Found";
    default: return L"Internal Server Error";
    }
}
} // namespace

void WebView::fail(HRESULT hr, const wchar_t *step)
{
    std::wostringstream out;
    out << step << L" 失败 (0x" << std::hex << hr << L")。请确认已安装 Microsoft Edge WebView2 Runtime。";
    if (on_error)
        on_error(out.str());
}
void WebView::resize(RECT bounds)
{
    bounds_ = bounds;
    if (controller_)
        controller_->put_Bounds(bounds);
}
void WebView::send(const Json &message)
{
    if (view_)
        view_->PostWebMessageAsJson(widen(message.dump(-1, ' ', false, Json::error_handler_t::replace)).c_str());
}

void WebView::create(HWND window, const fs::path &web_root, const fs::path &data_root)
{
    auto hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, (data_root / "webview-profile").c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, window, web_root](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(result) || !env)
                {
                    fail(result, L"WebView2 初始化");
                    return S_OK;
                }
                environment_ = env;
                auto requested = env->CreateCoreWebView2Controller(
                    window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [this, web_root](HRESULT created, ICoreWebView2Controller *controller) -> HRESULT {
                                    if (FAILED(created) || !controller)
                                    {
                                        fail(created, L"WebView2 控制器创建");
                                        return S_OK;
                                    }
                                    controller_ = controller;
                                    controller_->get_CoreWebView2(&view_);
                                    // Transparent until the page paints, so the window's theme background shows
                                    // through instead of a white flash (as in DR Reader).
                                    ComPtr<ICoreWebView2Controller2> controller2;
                                    if (SUCCEEDED(controller_.As(&controller2)))
                                        controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
                                    controller_->put_Bounds(bounds_);
                                    configure(web_root);
                                    return S_OK;
                                })
                                .Get());
                if (FAILED(requested))
                    fail(requested, L"WebView2 控制器请求");
                return S_OK;
            })
            .Get());
    if (FAILED(hr))
        fail(hr, L"WebView2 环境请求");
}

void WebView::configure(const fs::path &web_root)
{
    EventRegistrationToken token{};
    controller_->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *args) -> HRESULT {
                UINT key = 0;
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                args->get_VirtualKey(&key);
                args->get_KeyEventKind(&kind);
                args->get_PhysicalKeyStatus(&status);
                if (key == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000) &&
                    (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN))
                {
                    args->put_Handled(TRUE);
                    if (!status.WasKeyDown && on_toggle_borderless)
                        on_toggle_borderless();
                }
                return S_OK;
            })
            .Get(),
        &token);
    ComPtr<ICoreWebView2_3> v3;
    auto h = view_.As(&v3);
    if (FAILED(h))
    {
        fail(h, L"WebView2 虚拟目录支持");
        return;
    }
    h = v3->SetVirtualHostNameToFolderMapping(L"app.dr-dict.local", web_root.c_str(),
                                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(h))
    {
        fail(h, L"界面资源映射");
        return;
    }
    ComPtr<ICoreWebView2Settings> settings;
    view_->get_Settings(&settings);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
    settings->put_AreDevToolsEnabled(FALSE);
#endif
    ComPtr<ICoreWebView2Settings3> settings3;
    // F5, Ctrl+F, Ctrl+P and similar browser shortcuts do not belong to a dictionary window.
    if (SUCCEEDED(settings.As(&settings3)))
        settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    ComPtr<ICoreWebView2Settings5> settings5;
    if (SUCCEEDED(settings.As(&settings5)))
        settings5->put_IsPinchZoomEnabled(FALSE);

    // Keep only editing commands in the context menu so article text can still be copied.
    ComPtr<ICoreWebView2_11> v11;
    if (SUCCEEDED(view_.As(&v11)))
        v11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2ContextMenuRequestedEventArgs *args) -> HRESULT {
                    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
                    if (FAILED(args->get_MenuItems(&items)))
                        return S_OK;
                    UINT32 count = 0;
                    items->get_Count(&count);
                    for (UINT32 i = count; i-- > 0;)
                    {
                        ComPtr<ICoreWebView2ContextMenuItem> item;
                        if (FAILED(items->GetValueAtIndex(i, &item)))
                            continue;
                        LPWSTR raw = nullptr;
                        item->get_Name(&raw);
                        auto name = take(raw);
                        if (name != L"copy" && name != L"cut" && name != L"paste" && name != L"selectAll")
                            items->RemoveValueAtIndex(i);
                    }
                    items->get_Count(&count);
                    if (count == 0)
                        args->put_Handled(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);

    view_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                LPWSTR source = nullptr, raw = nullptr;
                args->get_Source(&source);
                args->get_WebMessageAsJson(&raw);
                auto origin = take(source);
                auto text = narrow(take(raw));
                if (origin != app_index || text.size() > 256 * 1024)
                    return S_OK;
                try
                {
                    if (on_message)
                        on_message(Json::parse(text));
                }
                catch (const std::exception &)
                { /* malformed messages never cross the native boundary */
                }
                return S_OK;
            })
            .Get(),
        &token);
    view_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (take(uri) != app_index)
                    args->put_Cancel(TRUE);
                return S_OK;
            })
            .Get(),
        &token);
    // Article frames may only show dictionary pages; external links are opened by the app.
    view_->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                auto target = take(uri);
                if (!target.starts_with(dict_origin) && target != L"about:blank")
                    args->put_Cancel(TRUE);
                return S_OK;
            })
            .Get(),
        &token);
    view_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &token);

    view_->AddWebResourceRequestedFilter(L"https://dict.dr-dict.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    view_->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *raw_args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args = raw_args;
                ComPtr<ICoreWebView2WebResourceRequest> request;
                args->get_Request(&request);
                LPWSTR raw_uri = nullptr;
                request->get_Uri(&raw_uri);
                auto uri = take(raw_uri);
                ComPtr<ICoreWebView2Deferral> deferral;
                if (!on_resource || FAILED(args->GetDeferral(&deferral)))
                    return S_OK;
                on_resource(uri, [this, args, deferral](ResourceReply reply) {
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(reply.body.data()),
                                                    static_cast<UINT>(reply.body.size())));
                    std::wstring headers = L"Content-Type: " + widen(reply.mime) +
                                           L"\r\nCache-Control: no-cache\r\nAccess-Control-Allow-Origin: *";
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (environment_ && SUCCEEDED(environment_->CreateWebResourceResponse(
                                            stream.Get(), reply.status, reason(reply.status), headers.c_str(), &response)))
                        args->put_Response(response.Get());
                    deferral->Complete();
                });
                return S_OK;
            })
            .Get(),
        &token);
    view_->Navigate(app_index);
}
} // namespace dr
