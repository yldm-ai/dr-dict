#include "http.h"
#include "dr/text.h"
#include <windows.h>
#include <winhttp.h>
#include <stdexcept>
namespace dr
{
namespace
{
struct Handle
{
    HINTERNET value = nullptr;
    explicit Handle(HINTERNET h) : value(h) {}
    ~Handle()
    {
        if (value)
            WinHttpCloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const { return value != nullptr; }
};
// The request handle is registered with the canceller, which may close it from another thread;
// whoever unregisters it closes it.
struct Request
{
    HINTERNET value;
    HttpCancel *owner;
    Request(HINTERNET h, HttpCancel *c) : value(h), owner(c)
    {
        if (value && owner && !owner->track(value))
        {
            WinHttpCloseHandle(value);
            value = nullptr;
        }
    }
    ~Request()
    {
        if (value && (!owner || owner->untrack(value)))
            WinHttpCloseHandle(value);
    }
    Request(const Request &) = delete;
    Request &operator=(const Request &) = delete;
    explicit operator bool() const { return value != nullptr; }
};
std::string host_name(const std::wstring &host)
{
    return lower_ascii(narrow(host));
}
} // namespace

void HttpCancel::cancel()
{
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    // Closing a request handle makes the blocking WinHTTP call on it return at once.
    for (auto *handle : open_)
        WinHttpCloseHandle(static_cast<HINTERNET>(handle));
    open_.clear();
}
bool HttpCancel::cancelled() const
{
    std::lock_guard lock(mutex_);
    return cancelled_;
}
bool HttpCancel::track(void *handle)
{
    std::lock_guard lock(mutex_);
    if (cancelled_)
        return false;
    open_.insert(handle);
    return true;
}
bool HttpCancel::untrack(void *handle)
{
    std::lock_guard lock(mutex_);
    return open_.erase(handle) > 0;
}

HttpFetch winhttp_fetch(std::set<std::string> hosts, std::shared_ptr<HttpCancel> cancel)
{
    return [hosts = std::move(hosts), cancel = std::move(cancel)](const HttpRequest &request) {
        if (cancel && cancel->cancelled())
            throw std::runtime_error("请求已取消");
        const auto url = widen(request.url);
        URL_COMPONENTS parts{sizeof(parts)};
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS)
            throw std::runtime_error("无效的网址");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        if (!hosts.count(host_name(host)))
            throw std::runtime_error("不允许访问的地址：" + narrow(host));
        const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                                  std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
        // Wikimedia asks API clients to name themselves in the User-Agent.
        Handle session(WinHttpOpen(L"DR-Dict/0.1 (Windows desktop dictionary)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session)
            throw std::runtime_error("无法初始化网络");
        WinHttpSetTimeouts(session.value, 5000, 5000, 8000, static_cast<int>(request.timeout_ms));
        Handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
        if (!connection)
            throw std::runtime_error("无法连接 " + narrow(host));
        Request handle(WinHttpOpenRequest(connection.value, widen(request.method).c_str(), path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                       cancel.get());
        if (cancel && cancel->cancelled())
            throw std::runtime_error("请求已取消");
        if (!handle)
            throw std::runtime_error("无法连接 " + narrow(host));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(handle.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(handle.value, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
        std::wstring headers;
        for (const auto &[name, value] : request.headers)
            headers += widen(name) + L": " + widen(value) + L"\r\n";
        const BOOL sent = WinHttpSendRequest(
            handle.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(-1L),
            request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(request.body.data()),
            static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(handle.value, nullptr))
        {
            if (cancel && cancel->cancelled())
                throw std::runtime_error("请求已取消");
            throw std::runtime_error(GetLastError() == ERROR_WINHTTP_TIMEOUT ? "连接 " + narrow(host) + " 超时"
                                                                             : "无法连接 " + narrow(host));
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        HttpResponse response{static_cast<int>(status), {}};
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(handle.value, &available))
                throw std::runtime_error("下载失败：" + narrow(host));
            if (!available)
                break;
            if (response.body.size() + available > request.limit)
                throw std::runtime_error("响应过大：" + narrow(host));
            const auto offset = response.body.size();
            response.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(handle.value, response.body.data() + offset, available, &read))
                throw std::runtime_error("下载失败：" + narrow(host));
            response.body.resize(offset + read);
        }
        return response;
    };
}
} // namespace dr
