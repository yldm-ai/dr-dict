#include "application.h"
#include "../resource.h"
#include "dr/text.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <stdexcept>
namespace dr
{
namespace
{
std::map<std::string, std::string> query_parameters(std::string_view query)
{
    std::map<std::string, std::string> result;
    size_t start = 0;
    while (start < query.size())
    {
        auto end = query.find('&', start);
        auto part = query.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        auto eq = part.find('=');
        if (eq != std::string_view::npos)
            result[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}
std::vector<fs::path> shell_items(IShellItemArray *items)
{
    std::vector<fs::path> files;
    DWORD count = 0;
    if (FAILED(items->GetCount(&count)))
        throw std::runtime_error("无法取得选中的文件");
    for (DWORD i = 0; i < count; ++i)
    {
        Microsoft::WRL::ComPtr<IShellItem> item;
        LPWSTR path = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            throw std::runtime_error("无法取得选中文件的路径");
        files.emplace_back(path);
        CoTaskMemFree(path);
    }
    return files;
}
} // namespace

Worker::Worker(size_t threads)
{
    for (size_t i = 0; i < threads; ++i)
        threads_.emplace_back([this](std::stop_token stop) {
            while (true)
            {
                std::function<void()> work;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait(lock, stop, [this] { return !queue_.empty(); });
                    if (stop.stop_requested() || queue_.empty())
                        return;
                    work = std::move(queue_.front());
                    queue_.pop();
                }
                work();
            }
        });
}
Worker::~Worker()
{
    for (auto &t : threads_)
        t.request_stop();
    wake_.notify_all();
}
void Worker::enqueue(std::function<void()> work)
{
    {
        std::lock_guard lock(mutex_);
        queue_.push(std::move(work));
    }
    wake_.notify_one();
}

Application::Application(fs::path root, fs::path assets, std::vector<fs::path> files, std::string shell)
    : library_(std::move(root)), assets_(std::move(assets)), initial_files_(std::move(files)),
      shell_override_(std::move(shell))
{
}

void Application::reply(const Json &id, Json result)
{
    window_.post([this, id, result = std::move(result)] { webview_.send({{"id", id}, {"result", result}}); });
}
void Application::reply_error(const Json &id, const std::string &error)
{
    window_.post([this, id, error] { webview_.send({{"id", id}, {"error", error}}); });
}

void Application::apply_settings(const Json &s)
{
    auto shell = shell_override_.empty() ? s.value("shell", std::string("mica")) : shell_override_;
    window_.set_style(shell == "direct2d" ? ShellStyle::Direct2D : ShellStyle::Mica);
    window_.set_theme(s.value("theme", std::string("system")));
}

std::vector<fs::path> Application::pick_files()
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法打开文件选择器");
    COMDLG_FILTERSPEC filters[] = {{L"词典文件 (*.mdx; *.ifo)", L"*.mdx;*.ifo"},
                                   {L"MDict 词典 (*.mdx)", L"*.mdx"},
                                   {L"StarDict 词典 (*.ifo)", L"*.ifo"}};
    dialog->SetFileTypes(3, filters);
    dialog->SetTitle(L"添加词典");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    auto hr = dialog->Show(window_.handle());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(hr) || FAILED(dialog->GetResults(&items)))
        throw std::runtime_error("文件选择失败");
    return shell_items(items.Get());
}

fs::path Application::pick_folder()
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法打开文件夹选择器");
    dialog->SetTitle(L"选择包含词典的文件夹");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    auto hr = dialog->Show(window_.handle());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    LPWSTR path = nullptr;
    if (FAILED(hr) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        throw std::runtime_error("文件夹选择失败");
    fs::path folder(path);
    CoTaskMemFree(path);
    return folder;
}

void Application::import(const Json &id, std::vector<fs::path> paths)
{
    imports_.enqueue([this, id, paths = std::move(paths)] {
        try
        {
            window_.post([this] { webview_.send({{"event", "import-start"}}); });
            auto result = library_.add(paths, [this](const Json &progress) {
                window_.post([this, progress] {
                    auto event = progress;
                    event["event"] = "import-progress";
                    webview_.send(event);
                });
            });
            window_.post([this, result] {
                auto event = result;
                event["event"] = "import-done";
                webview_.send(event);
            });
            if (!id.is_null())
                reply(id, result);
        }
        catch (const std::exception &e)
        {
            window_.post([this] { webview_.send({{"event", "import-done"}, {"errors", Json::array()}}); });
            if (!id.is_null())
                reply_error(id, e.what());
        }
    });
}

void Application::receive(const Json &request)
{
    if (!request.is_object() || !request.contains("id") || !request["id"].is_number_unsigned() ||
        !request.contains("method") || !request["method"].is_string())
        return;
    const auto id = request["id"];
    const auto method = request["method"].get<std::string>();
    const auto params = request.value("params", Json::object());
    // Dialogs and window state live on the UI thread.
    try
    {
        if (method == "dict.addFiles" || method == "dict.addFolder")
        {
            std::vector<fs::path> paths;
            if (method == "dict.addFiles")
                paths = pick_files();
            else if (auto folder = pick_folder(); !folder.empty())
                paths.push_back(folder);
            if (paths.empty())
                webview_.send({{"id", id}, {"result", {{"cancelled", true}}}});
            else
                import(id, std::move(paths));
            return;
        }
        if (method == "open.external")
        {
            auto url = params.at("url").get<std::string>();
            if (!url.starts_with("https://") && !url.starts_with("http://") && !url.starts_with("mailto:"))
                throw std::runtime_error("只能打开网页链接");
            ShellExecuteW(window_.handle(), L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            webview_.send({{"id", id}, {"result", true}});
            return;
        }
        if (method == "window.title")
        {
            auto word = trim(params.value("word", std::string()));
            window_.set_title(word.empty() ? L"DR Dict" : widen(word) + L" — DR Dict");
            webview_.send({{"id", id}, {"result", true}});
            return;
        }
        if (method == "window.dim")
        {
            window_.set_dimmed(params.value("dimmed", false));
            webview_.send({{"id", id}, {"result", true}});
            return;
        }
        if (method == "window.close")
        {
            DestroyWindow(window_.handle());
            return;
        }
    }
    catch (const std::exception &e)
    {
        webview_.send({{"id", id}, {"error", e.what()}});
        return;
    }
    const bool writes_index = method == "dict.remove" || method == "dict.reindex";
    auto &worker = writes_index ? imports_ : queries_;
    worker.enqueue([this, id, method, params] {
        try
        {
            Json result;
            if (method == "bootstrap")
            {
                result = {{"dictionaries", library_.list()},
                          {"settings", library_.settings()},
                          {"defaults", Library::default_settings()},
                          {"history", library_.history()},
                          {"dataPath", utf8(library_.root())},
                          // The page always sees prefers-color-scheme: light (see WebView::configure),
                          // so "follow system" takes the Windows theme from here.
                          {"systemDark", Window::system_dark()}};
                if (!initial_files_.empty())
                {
                    auto files = std::move(initial_files_);
                    initial_files_.clear();
                    window_.post([this, files] { import(nullptr, files); });
                }
            }
            else if (method == "suggest")
                result = library_.suggest(params.at("query").get<std::string>(),
                                          std::min<size_t>(params.value("limit", 60), 200));
            else if (method == "lookup")
            {
                auto word = params.at("word").get<std::string>();
                result = library_.lookup(word);
                if (!result["results"].empty() && params.value("remember", true))
                    result["history"] = library_.remember(result.value("word", word));
            }
            else if (method == "history.clear")
                result = library_.clear_history();
            else if (method == "dict.list")
                result = library_.list();
            else if (method == "dict.update")
                result = library_.update(params.at("id"), params.at("patch"));
            else if (method == "dict.reorder")
                result = library_.reorder(params.at("ids"));
            else if (method == "dict.info")
                result = library_.info(params.at("id"));
            else if (method == "dict.remove")
            {
                library_.remove(params.at("id"));
                result = library_.list();
            }
            else if (method == "dict.reindex")
            {
                auto dict_id = params.at("id").get<std::string>();
                result = library_.reindex(dict_id, [this](const Json &progress) {
                    window_.post([this, progress] {
                        auto event = progress;
                        event["event"] = "import-progress";
                        webview_.send(event);
                    });
                });
            }
            else if (method == "settings.save")
            {
                result = library_.save_settings(params);
                window_.post([this, result] { apply_settings(result); });
            }
            else
                throw std::runtime_error("未知命令: " + method);
            reply(id, std::move(result));
        }
        catch (const std::exception &e)
        {
            reply_error(id, e.what());
        }
    });
}

// https://dict.dr-dict.local/<id>/@article?word=…&theme=… | /<id>/@about | /<id>/@icon | /<id>/<resource>
void Application::resource(const std::wstring &wide_uri, std::function<void(ResourceReply)> respond)
{
    auto uri = narrow(wide_uri);
    const std::string origin = narrow(dict_origin);
    queries_.enqueue([this, uri, origin, respond = std::move(respond)] {
        ResourceReply reply;
        try
        {
            auto rest = uri.substr(origin.size());
            auto slash = rest.find('/');
            auto id = rest.substr(0, slash);
            auto path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
            auto query_at = path.find('?');
            auto query = query_parameters(query_at == std::string::npos ? "" : std::string_view(path).substr(query_at + 1));
            auto route = path.substr(0, query_at);
            auto theme = query.count("theme") ? query["theme"] : std::string("light");
            if (theme != "dark" && theme != "light")
                theme = "light";
            if (route == "@article")
            {
                reply = {200, "text/html; charset=utf-8", library_.article_page(id, query["word"], theme)};
            }
            else if (route == "@about")
                reply = {200, "text/html; charset=utf-8", library_.about_page(id, theme)};
            else if (route == "@icon")
            {
                if (auto icon = library_.icon(id))
                    reply = {200, icon->mime, std::move(icon->data)};
            }
            else if (auto found = library_.resource(id, path))
                reply = {200, found->mime, std::move(found->data)};
        }
        catch (const std::exception &e)
        {
            reply = {500, "text/plain; charset=utf-8", e.what()};
        }
        window_.post([respond, reply = std::move(reply)] { respond(reply); });
    });
}

int Application::run(HINSTANCE instance)
{
    auto settings = library_.settings();
    auto shell = shell_override_.empty() ? settings.value("shell", std::string("mica")) : shell_override_;
    if (!window_.create(instance, shell == "direct2d" ? ShellStyle::Direct2D : ShellStyle::Mica, IDI_DR_DICT,
                        settings["window"].value("widthRatio", 0.4), settings["window"].value("heightRatio", 0.5),
                        settings.value("theme", std::string("system"))))
        return 1;
    window_.set_title(L"DR Dict");
    window_.on_resize = [this] { webview_.resize(window_.content_bounds()); };
    window_.on_toggle_borderless = [this] {
        window_.post([this] {
            window_.set_borderless(!window_.borderless());
            webview_.send({{"event", "titlebar"}, {"hidden", window_.borderless()}});
        });
    };
    webview_.on_toggle_borderless = window_.on_toggle_borderless;
    window_.on_system_theme = [this](bool dark) { webview_.send({{"event", "system-theme"}, {"dark", dark}}); };
    window_.on_caption_tooltip = [this](const std::wstring &text, RECT button) {
        const double scale = 96.0 / GetDpiForWindow(window_.handle());
        webview_.send({{"event", "caption-tooltip"},
                       {"text", narrow(text)},
                       {"left", button.left * scale},
                       {"width", (button.right - button.left) * scale}});
    };
    window_.set_theme(settings.value("theme", std::string("system")));
    webview_.on_message = [this](const Json &request) { receive(request); };
    webview_.on_resource = [this](const std::wstring &uri, std::function<void(ResourceReply)> respond) {
        resource(uri, std::move(respond));
    };
    webview_.on_error = [this](const std::wstring &error) {
        MessageBoxW(window_.handle(), error.c_str(), L"DR Dict", MB_OK | MB_ICONERROR);
    };
    webview_.resize(window_.content_bounds());
    webview_.create(window_.handle(), assets_, library_.root());
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
} // namespace dr
