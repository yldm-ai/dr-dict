#include "application.h"
#include "../resource.h"
#include "dr/text.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <dpapi.h>
#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cwctype>
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

Application::Application(fs::path root, fs::path assets, std::vector<fs::path> files, std::string shell,
                         std::optional<LookupRequest> lookup)
    : pending_lookup_(std::move(lookup)), library_(std::move(root)), assets_(std::move(assets)),
      initial_files_(std::move(files)), shell_override_(std::move(shell)),
      niutrans_(winhttp_fetch({NiuTrans::host}, http_cancel_)),
      wiktionary_(winhttp_fetch({"en.wiktionary.org", "zh.wiktionary.org"}, http_cancel_))
{
    // OpenCC data is installed beside the web assets (bin/<config>/opencc).
    library_.set_opencc_dir(assets_.parent_path() / "opencc");
    load_niutrans_key();
}

namespace
{
fs::path niutrans_key_file(const fs::path &root)
{
    return root / "niutrans-key.bin";
}
// DPAPI (CryptProtectData) ties the secret to this Windows user, as in DR Music's
// Application::seal / unseal. An empty secret removes the file.
void seal(const fs::path &file, const std::string &secret)
{
    std::error_code ec;
    if (secret.empty())
    {
        fs::remove(file, ec);
        return;
    }
    DATA_BLOB input{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE *>(const_cast<char *>(secret.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"DR Dict NiuTrans key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("无法加密保存密钥");
    std::string sealed(reinterpret_cast<char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    write_atomic(file, sealed);
}
std::string unseal(const fs::path &file)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > 64 * 1024)
        return {};
    try
    {
        auto sealed = read_file(file);
        DATA_BLOB input{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE *>(sealed.data())};
        DATA_BLOB output{};
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
            return {};
        std::string secret(reinterpret_cast<char *>(output.pbData), output.cbData);
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return secret;
    }
    catch (const std::exception &)
    {
        return {};
    }
}
} // namespace

void Application::load_niutrans_key()
{
    const auto json = Json::parse(unseal(niutrans_key_file(library_.root())), nullptr, false);
    niutrans_.set_credentials(json.is_object() ? json.value("appId", "") : "",
                              json.is_object() ? json.value("apikey", "") : "");
}
void Application::save_niutrans_key(const std::string &app_id, const std::string &apikey)
{
    auto plain = [](const std::string &s) {
        return s.size() <= 128 &&
               std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
    };
    if (!plain(app_id) || !plain(apikey) || app_id.empty() != apikey.empty())
        throw std::runtime_error("App ID 或 API Key 格式不正确");
    seal(niutrans_key_file(library_.root()), app_id.empty() ? "" : Json{{"appId", app_id}, {"apikey", apikey}}.dump());
    load_niutrans_key();
}
Json Application::niutrans_status() const
{
    return {{"configured", niutrans_.configured()}, {"appId", niutrans_.app_id()}};
}

bool Application::receive_agent(const Json &id, const std::string &method, const Json &params, WebView &from)
{
    static constexpr AgentKind kinds[] = {AgentKind::Codex, AgentKind::Claude, AgentKind::OpenCode};
    try
    {
        if (method == "agent.detect")
        {
            // Looking through PATH touches the disk and registry; keep it off the UI thread.
            queries_.enqueue([this, &from, id] {
                Json result = Json::object();
                for (auto kind : kinds)
                {
                    auto found = AgentRunner::find(kind);
                    result[agent_id(kind)] = found ? Json{{"path", utf8(found->path)}} : Json(nullptr);
                }
                reply(from, id, result);
            });
            return true;
        }
        if (method == "agent.cancel")
        {
            if (agents_)
                agents_->cancel(params.at("request").get<std::string>());
            from.send({{"id", id}, {"result", true}});
            return true;
        }
        auto runner = [this]() -> AgentRunner & {
            if (!agents_)
                agents_ = std::make_unique<AgentRunner>([this](const AgentUpdate &update) {
                    static constexpr const char *types[] = {"text", "status", "done", "error"};
                    Json event{{"event", "agent"},
                               {"request", update.request},
                               {"type", types[static_cast<int>(update.type)]},
                               {"text", update.text}};
                    window_.post([this, event = std::move(event)] { webview_.send(event); });
                });
            return *agents_;
        };
        if (method == "agent.models")
        {
            if (!library_.settings().value("agentEnabled", false))
                throw std::runtime_error("AI 助查没有开启，请在“设置 → AI 助查”中打开");
            // Runs the CLI for a few seconds without a model turn: on the runner's own thread.
            auto kind = agent_kind(params.value("agent", std::string()));
            if (!kind)
                throw std::runtime_error("请先选择具体的工具");
            auto agent = AgentRunner::find(*kind);
            if (!agent)
                throw std::runtime_error(std::string("没有找到 ") + agent_label(*kind) + "，请确认已安装并能在终端中运行");
            runner().list_models(*agent, [this, &from, id](std::vector<AgentModel> models, std::string error) {
                if (!error.empty())
                    return reply_error(from, id, error);
                Json list = Json::array();
                for (auto &model : models)
                    list.push_back({{"id", model.id},
                                    {"name", model.name},
                                    {"description", model.description},
                                    {"default", model.is_default},
                                    {"efforts", model.efforts},
                                    {"defaultEffort", model.default_effort}});
                reply(from, id, {{"models", list}});
            });
            return true;
        }
        if (method != "agent.ask")
            return false;
        if (!library_.settings().value("agentEnabled", false))
            throw std::runtime_error("AI 助查没有开启，请在“设置 → AI 助查”中打开");
        const auto request = params.at("request").get<std::string>();
        const auto prompt = params.at("prompt").get<std::string>();
        const auto choice = params.value("agent", std::string("auto"));
        const auto model = params.value("model", std::string());
        const auto effort = params.value("effort", std::string());
        if (request.empty() || request.size() > 64)
            throw std::runtime_error("提问标识无效");
        if (prompt.empty() || prompt.size() > (1u << 20))
            throw std::runtime_error("问题为空或附带的内容过长");
        std::optional<AgentExecutable> agent;
        if (choice == "auto")
        {
            for (auto kind : kinds)
                if ((agent = AgentRunner::find(kind)))
                    break;
            if (!agent)
                throw std::runtime_error("没有找到 Codex、Claude Code 或 opencode，请先安装其中一个并登录");
        }
        else
        {
            auto kind = agent_kind(choice);
            if (!kind)
                throw std::runtime_error("不支持的 Agent");
            agent = AgentRunner::find(*kind);
            if (!agent)
                throw std::runtime_error(std::string("没有找到 ") + agent_label(*kind) + "，请确认已安装并能在终端中运行");
        }
        runner().start(request, *agent, model, effort, prompt);
        from.send({{"id", id}, {"result", {{"agent", agent_id(agent->kind)}, {"label", agent_label(agent->kind)}}}});
    }
    catch (const std::exception &error)
    {
        from.send({{"id", id}, {"error", error.what()}});
    }
    return true;
}

void Application::online(const Json &id, const std::string &method, const Json &params, WebView &from)
{
    online_.enqueue([this, &from, id, method, params] {
        try
        {
            Json result;
            if (method == "translate")
                result = niutrans_.translate(params.at("text").get<std::string>());
            else if (method == "niutrans.test")
                result = niutrans_.translate("今天天气很好，我们去公园散步吧。", false); // not from memory
            else if (method == "wiki.lookup")
            {
                // {word, site} -> {found, id, site, name, title}; the frame then loads @wiki-<site>/@article.
                const auto site_id = params.at("site").get<std::string>();
                const auto *site = find_wiki_site(site_id);
                if (!site)
                    throw std::runtime_error("未知的维基词典");
                auto article = wiktionary_.lookup(*site, params.at("word").get<std::string>());
                result = {{"found", article.has_value()}, {"id", "@wiki-" + site->id}, {"site", site->id}, {"name", site->name}};
                if (article)
                    result["title"] = article->title;
            }
            reply(from, id, std::move(result));
        }
        catch (const std::exception &e)
        {
            reply_error(from, id, e.what());
        }
    });
}

void Application::reply(WebView &to, const Json &id, Json result)
{
    window_.post([&to, id, result = std::move(result)] { to.send({{"id", id}, {"result", result}}); });
}
void Application::reply_error(WebView &to, const Json &id, const std::string &error)
{
    window_.post([&to, id, error] { to.send({{"id", id}, {"error", error}}); });
}

void Application::apply_settings(const Json &s)
{
    auto shell = shell_override_.empty() ? s.value("shell", std::string("mica")) : shell_override_;
    window_.set_style(shell == "direct2d" ? ShellStyle::Direct2D : ShellStyle::Mica);
    // The page follows with the exact colours (window.chrome) once it has applied them.
    window_.set_theme(theme_mode(s.value("theme", std::string("system"))));
    apply_hotkey(s);
}

void Application::apply_hotkey(const Json &s)
{
    if (s.value("scanPopup", true))
    {
        if (!hotkey_.installed())
            hotkey_.install([this] { window_.post([this] { copied_twice(); }); });
    }
    else
    {
        hotkey_.uninstall();
        popup_.hide();
    }
}

namespace
{
// Text to look up in the popup: whitespace runs collapsed and trimmed, at most 256 characters.
std::string popup_word(const std::wstring &text)
{
    std::wstring collapsed;
    for (wchar_t c : text)
    {
        const bool space = iswspace(c) || c == 0x3000;
        if (space && (collapsed.empty() || collapsed.back() == L' '))
            continue;
        collapsed += space ? L' ' : c;
    }
    while (!collapsed.empty() && collapsed.back() == L' ')
        collapsed.pop_back();
    // A paragraph is translated (小牛翻译 takes 5000 characters a request); beyond that the popup
    // still opens with its start.
    if (collapsed.size() > 4000)
        collapsed.resize(4000);
    if (!collapsed.empty() && IS_HIGH_SURROGATE(collapsed.back()))
        collapsed.pop_back();
    return narrow(collapsed);
}
// The clipboard's text as popup_word; empty when it holds none. Another application may still
// have the clipboard open right after its copy, so opening is retried.
std::string clipboard_text()
{
    bool open = false;
    for (int attempt = 0; attempt < 10 && !(open = OpenClipboard(nullptr)); ++attempt)
        Sleep(20);
    if (!open)
        return {};
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
        if (auto *chars = static_cast<const wchar_t *>(GlobalLock(data)))
        {
            text.assign(chars, wcsnlen(chars, GlobalSize(data) / sizeof(wchar_t)));
            GlobalUnlock(data);
        }
    CloseClipboard();
    return popup_word(text);
}
} // namespace

// Runs on a worker: the popup gets the word with the settings and dictionaries it renders with.
void Application::send_popup_lookup(const std::string &word)
{
    Json event = {{"event", "popup-lookup"},
                  {"word", word},
                  {"settings", library_.settings()},
                  {"dictionaries", library_.list()},
                  {"favorites", library_.favorites()},
                  {"systemDark", Window::system_dark()},
                  {"chineseAvailable", library_.chinese_available()},
                  {"niutrans", niutrans_status()}};
    // The page looks the word up, renders, then asks for popup.show.
    window_.post([this, event] { popup_view_.send(event); });
}

std::string lookup_payload(const LookupRequest &request)
{
    Json payload{{"word", request.word}, {"x", request.anchor.point.x}, {"y", request.anchor.point.y}};
    if (const auto &caret = request.anchor.caret)
        payload["caret"] = Json::array({caret->left, caret->top, caret->right, caret->bottom});
    if (request.anchor.keyboard)
        payload["keyboard"] = true;
    return payload.dump();
}

std::optional<LookupRequest> parse_lookup_payload(std::string_view text)
{
    auto request = Json::parse(text, nullptr, false);
    if (!request.is_object() || !request.contains("word") || !request["word"].is_string())
        return std::nullopt;
    LookupRequest result{request["word"].get<std::string>()};
    GetCursorPos(&result.anchor.point);
    if (request.contains("x") && request["x"].is_number_integer() && request.contains("y") &&
        request["y"].is_number_integer())
        result.anchor.point = {request["x"].get<LONG>(), request["y"].get<LONG>()};
    if (request.contains("caret") && request["caret"].is_array() && request["caret"].size() == 4 &&
        std::all_of(request["caret"].begin(), request["caret"].end(),
                    [](const Json &value) { return value.is_number_integer(); }))
    {
        const auto &c = request["caret"];
        const RECT caret{c[0].get<LONG>(), c[1].get<LONG>(), c[2].get<LONG>(), c[3].get<LONG>()};
        if (caret.left <= caret.right && caret.top <= caret.bottom)
            result.anchor.caret = caret;
    }
    result.anchor.keyboard = request.contains("keyboard") && request["keyboard"].is_boolean() &&
                             request["keyboard"].get<bool>();
    return result;
}

std::optional<RECT> parse_caret_argument(const std::wstring &text)
{
    long values[4]{};
    size_t start = 0;
    for (int i = 0; i < 4; ++i)
    {
        const size_t end = i < 3 ? text.find(L',', start) : text.size();
        if (end == std::wstring::npos || end == start)
            return std::nullopt;
        const std::wstring part = text.substr(start, end - start);
        wchar_t *stop = nullptr;
        values[i] = std::wcstol(part.c_str(), &stop, 10);
        if (!stop || *stop != L'\0')
            return std::nullopt;
        start = end + 1;
    }
    const RECT caret{values[0], values[1], values[2], values[3]};
    if (caret.left > caret.right || caret.top > caret.bottom)
        return std::nullopt;
    return caret;
}

void Application::request_lookup(LookupRequest request)
{
    request.word = popup_word(widen(request.word));
    if (request.word.empty())
        return;
    // Started by the request (--lookup), or asked right after start-up: wait for the popup's
    // WebView and its TOPMOST (see create_popup), then show it.
    if (!popup_view_.ready() || !popup_.topmost())
    {
        pending_lookup_ = std::move(request);
        return;
    }
    popup_anchor_ = request.anchor;
    queries_.enqueue([this, word = std::move(request.word)] { send_popup_lookup(word); });
}

void Application::copied_twice()
{
    if (!popup_view_.ready())
        return;
    // Ctrl+C+C inside DR Dict itself is only a copy.
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    if (process == GetCurrentProcessId() && GetForegroundWindow() != popup_.handle())
        return;
    popup_anchor_ = {};
    GetCursorPos(&popup_anchor_.point);
    queries_.enqueue([this] {
        // The first Ctrl+C was delivered before ours; give the application a moment to put
        // the selection on the clipboard.
        Sleep(80);
        auto word = clipboard_text();
        if (!word.empty())
            send_popup_lookup(word);
    });
}

void Application::create_popup(HINSTANCE instance, const Json &settings)
{
    if (!popup_.create(instance, window_.handle()))
        return;
    popup_.set_size(settings.value("popupWidth", 360), settings.value("popupHeight", 420));
    popup_.on_bounds = [this](RECT bounds) { popup_view_.resize(bounds); };
    popup_.on_moved = [this] { popup_view_.notify_moved(); };
    // A hidden host's WebView is told it is hidden and shown again with the host, which makes it
    // paint a fresh frame (as MSIME-Windows re-asserts visibility for its small windows).
    popup_.on_shown = [this] { popup_view_.set_visible(true); };
    popup_.on_focus = [this] { popup_view_.focus(); };
    popup_.on_hidden = [this] {
        popup_view_.set_visible(false);
        popup_view_.send({{"event", "popup-hidden"}});
    };
    popup_.on_resized = [this](int width, int height) {
        queries_.enqueue([this, width, height] { library_.save_settings({{"popupWidth", width}, {"popupHeight", height}}); });
    };
    popup_.on_topmost = [this] {
        if (auto request = std::exchange(pending_lookup_, std::nullopt))
            request_lookup(std::move(*request));
    };
    popup_.on_request = [this](std::string_view payload) {
        // {"word": "...", "x": …, "y": …, "caret": […], "keyboard": …} from another program (see popup.h).
        if (auto request = parse_lookup_payload(payload))
            request_lookup(std::move(*request));
    };
    // DR Dict started again (see main.cpp): come to the front and take the files it was given.
    popup_.on_open = [this](std::string_view payload) {
        auto request = Json::parse(payload, nullptr, false);
        std::vector<fs::path> files;
        if (request.is_object() && request.contains("files") && request["files"].is_array())
            for (const auto &file : request["files"])
                if (file.is_string())
                    files.emplace_back(widen(file.get<std::string>()));
        show_main();
        if (!files.empty())
            window_.post([this, files = std::move(files)] { import(nullptr, files); });
    };
    SetPropW(popup_.handle(), instance_property, reinterpret_cast<HANDLE>(instance_tag(library_.root())));
    popup_view_.on_message = [this](const Json &request) { receive(request, popup_view_); };
    popup_view_.on_resource = webview_.on_resource;
    popup_view_.on_error = [](const std::wstring &) {}; // the main window reports WebView2 problems
    // TOPMOST only once the WebView is up and has settled (see Popup): pinning a host while its
    // controller is created or bringing up its first frames breaks it under uiAccess.
    popup_view_.on_ready = [this] {
        popup_view_.set_visible(false);
        popup_.schedule_topmost(1000);
    };
    RECT client{};
    GetClientRect(popup_.handle(), &client);
    popup_view_.resize(client);
    popup_view_.create(popup_.handle(), assets_, library_.root(), app_popup, true);
}

fs::path Application::pick_image()
{
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("无法打开文件选择器");
    COMDLG_FILTERSPEC filters[] = {{L"图片 (*.jpg; *.png; *.webp; *.gif; *.bmp)", L"*.jpg;*.jpeg;*.png;*.webp;*.gif;*.bmp"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(L"选择背景图片");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    auto hr = dialog->Show(window_.handle());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    LPWSTR path = nullptr;
    if (FAILED(hr) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        throw std::runtime_error("图片选择失败");
    fs::path file(path);
    CoTaskMemFree(path);
    return file;
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

void Application::import(const Json &id, std::vector<fs::path> paths, bool rescan)
{
    imports_.enqueue([this, id, paths = std::move(paths), rescan] {
        try
        {
            window_.post([this] { webview_.send({{"event", "import-start"}}); });
            auto progress = [this](const Json &p) {
                window_.post([this, p] {
                    auto event = p;
                    event["event"] = "import-progress";
                    webview_.send(event);
                });
            };
            // An empty path list with `rescan` scans the remembered folders again.
            auto result = rescan ? library_.rescan(progress) : library_.add(paths, progress);
            window_.post([this, result] {
                auto event = result;
                event["event"] = "import-done";
                webview_.send(event);
            });
            if (!id.is_null())
                reply(webview_, id, result);
        }
        catch (const std::exception &e)
        {
            window_.post([this] { webview_.send({{"event", "import-done"}, {"errors", Json::array()}}); });
            if (!id.is_null())
                reply_error(webview_, id, e.what());
        }
    });
}

namespace
{
// Dictionary ids a search is limited to ("dicts": [...]); none or empty means all enabled ones.
std::vector<std::string> scope_of(const Json &params)
{
    std::vector<std::string> ids;
    if (params.contains("dicts") && params["dicts"].is_array())
        for (auto &id : params["dicts"])
            if (id.is_string())
                ids.push_back(id.get<std::string>());
    return ids;
}
} // namespace

void Application::receive(const Json &request, WebView &from)
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
                from.send({{"id", id}, {"result", {{"cancelled", true}}}});
            else
                import(id, std::move(paths));
            return;
        }
        if (method == "background.choose")
        {
            auto file = pick_image();
            if (file.empty())
            {
                from.send({{"id", id}, {"result", {{"cancelled", true}}}});
                return;
            }
            queries_.enqueue([this, &from, id, file] {
                try
                {
                    reply(from, id, library_.set_background_image(file));
                }
                catch (const std::exception &e)
                {
                    reply_error(from, id, e.what());
                }
            });
            return;
        }
        if (method == "window.chrome")
        {
            // Title bar colours matching the page's resolved scheme ("#rrggbb" each).
            auto color = [&](const char *key, UINT32 fallback) {
                auto text = params.value(key, std::string());
                if (text.size() != 7 || text[0] != '#')
                    return fallback;
                try
                {
                    return static_cast<UINT32>(std::stoul(text.substr(1), nullptr, 16));
                }
                catch (const std::exception &)
                {
                    return fallback;
                }
            };
            window_.set_caption_colors(params.value("dark", false), color("background", 0xffffff),
                                       color("text", 0x37352f), color("hover", 0xefefed));
            from.send({{"id", id}, {"result", true}});
            return;
        }
        if (method == "dict.rescan")
        {
            import(id, {}, true);
            return;
        }
        if (method.starts_with("agent.") && receive_agent(id, method, params, from))
            return;
        if (method == "translate" || method == "niutrans.test" || method == "wiki.lookup")
        {
            online(id, method, params, from);
            return;
        }
        if (method == "niutrans.save")
        {
            // {appId, apikey}; both empty clears the saved key.
            save_niutrans_key(params.at("appId").get<std::string>(), params.at("apikey").get<std::string>());
            from.send({{"id", id}, {"result", niutrans_status()}});
            return;
        }
        if (method == "open.external")
        {
            auto url = params.at("url").get<std::string>();
            if (!url.starts_with("https://") && !url.starts_with("http://") && !url.starts_with("mailto:"))
                throw std::runtime_error("只能打开网页链接");
            ShellExecuteW(window_.handle(), L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            from.send({{"id", id}, {"result", true}});
            return;
        }
        if (method == "window.zoom")
        {
            // Browser zoom steps (Chromium's), for Ctrl +, Ctrl − and Ctrl 0.
            static constexpr double steps[] = {0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1,
                                               1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0};
            const auto step = params.value("step", std::string("reset"));
            const double now = webview_.zoom();
            double next = 1.0;
            if (step == "in")
            {
                next = steps[std::size(steps) - 1];
                for (double s : steps)
                    if (s > now + 0.001)
                    {
                        next = s;
                        break;
                    }
            }
            else if (step == "out")
            {
                next = steps[0];
                for (double s : steps)
                    if (s < now - 0.001)
                        next = s;
            }
            webview_.set_zoom(next);
            from.send({{"id", id}, {"result", next}});
            return;
        }
        if (method == "window.dim")
        {
            window_.set_dimmed(params.value("dimmed", false));
            from.send({{"id", id}, {"result", true}});
            return;
        }
        if (method.starts_with("popup."))
        {
            Json result = true;
            if (method == "popup.show")
                popup_.show_at(popup_anchor_); // the page has rendered the new lookup
            else if (method == "popup.hide")
                popup_.hide();
            else if (method == "popup.pin")
            {
                popup_.set_pinned(params.value("pinned", false));
                result = popup_.pinned();
            }
            else if (method == "popup.chrome")
            {
                // The edge around the popup's WebView matches the page background.
                auto text = params.value("background", std::string("#ffffff"));
                UINT32 background = 0xffffff;
                if (text.size() == 7 && text[0] == '#')
                    background = static_cast<UINT32>(std::stoul(text.substr(1), nullptr, 16));
                popup_.set_colors(background, params.value("dark", false));
            }
            else if (method == "popup.openMain")
            {
                // Continue in the main window: restore it, bring it forward and look the word up.
                auto word = params.value("word", std::string());
                if (!popup_.pinned())
                    popup_.hide();
                show_main();
                if (!word.empty())
                    webview_.send({{"event", "lookup-word"}, {"word", word}});
            }
            else
                throw std::runtime_error("未知命令: " + method);
            from.send({{"id", id}, {"result", result}});
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
        from.send({{"id", id}, {"error", e.what()}});
        return;
    }
    const bool writes_index = method == "dict.remove" || method == "dict.reindex" || method == "dict.restore";
    auto &worker = writes_index ? imports_ : queries_;
    worker.enqueue([this, &from, id, method, params] {
        try
        {
            Json result;
            if (method == "bootstrap")
            {
                result = {{"dictionaries", library_.list()},
                          {"settings", library_.settings()},
                          {"defaults", Library::default_settings()},
                          {"history", library_.history()},
                          {"favorites", library_.favorites()},
                          {"dataPath", utf8(library_.root())},
                          // The page always sees prefers-color-scheme: light (see WebView::configure),
                          // so "follow system" takes the Windows theme from here.
                          {"systemDark", Window::system_dark()},
                          {"chineseAvailable", library_.chinese_available()},
                          {"niutrans", niutrans_status()}};
                if (!initial_files_.empty())
                {
                    auto files = std::move(initial_files_);
                    initial_files_.clear();
                    window_.post([this, files] { import(nullptr, files); });
                }
            }
            else if (method == "suggest")
                result = library_.suggest(params.at("query").get<std::string>(),
                                          std::min<size_t>(params.value("limit", 60), 200), scope_of(params));
            else if (method == "lookup")
            {
                auto word = params.at("word").get<std::string>();
                result = library_.lookup(word, scope_of(params));
                if (!result["results"].empty() && params.value("remember", true))
                {
                    result["history"] = library_.remember(result.value("word", word));
                    // A lookup in the popup also lands in the main window's recent list.
                    if (&from == &popup_view_)
                        window_.post([this, history = result["history"]] {
                            webview_.send({{"event", "history"}, {"history", history}});
                        });
                }
            }
            else if (method == "history.clear")
                result = library_.clear_history();
            else if (method == "background.clear")
                result = library_.clear_background_image();
            else if (method == "history.remove")
                result = library_.forget_word(params.at("word").get<std::string>());
            else if (method == "favorites.toggle")
            {
                result = library_.toggle_favorite(params.at("word").get<std::string>());
                // The popup's star updates the main window's favorites as well.
                if (&from == &popup_view_)
                    window_.post([this, favorites = result] {
                        webview_.send({{"event", "favorites"}, {"favorites", favorites}});
                    });
            }
            else if (method == "dict.list")
                result = library_.list();
            else if (method == "dict.folders")
                result = library_.folders();
            else if (method == "dict.removeFolder")
                result = library_.remove_folder(params.at("path").get<std::string>());
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
            else if (method == "dict.reindex" || method == "dict.restore")
            {
                auto dict_id = params.at("id").get<std::string>();
                auto progress = [this](const Json &p) {
                    window_.post([this, p] {
                        auto event = p;
                        event["event"] = "import-progress";
                        webview_.send(event);
                    });
                };
                result = method == "dict.reindex" ? library_.reindex(dict_id, progress)
                                                  : library_.restore(dict_id, progress);
            }
            else if (method == "dict.forget")
                result = library_.forget(params.at("id").get<std::string>());
            else if (method == "settings.save")
            {
                result = library_.save_settings(params);
                window_.post([this, result] { apply_settings(result); });
            }
            else
                throw std::runtime_error("未知命令: " + method);
            reply(from, id, std::move(result));
        }
        catch (const std::exception &e)
        {
            reply_error(from, id, e.what());
        }
    });
}

// https://dict.dr-dict.local/<id>/@article?word=…&theme=… | /<id>/@about | /<id>/@icon | /<id>/<resource>
void Application::resource(const std::wstring &wide_uri, std::function<void(ResourceReply)> respond)
{
    auto uri = narrow(wide_uri);
    const std::string origin = narrow(dict_origin);
    // Wiktionary frames (/@wiki-<site>/@article?word=…) may have to fetch the page first.
    const bool wiki = uri.compare(origin.size(), 6, "@wiki-") == 0;
    (wiki ? online_ : queries_).enqueue([this, uri, origin, respond = std::move(respond)] {
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
            if (id.starts_with("@wiki-"))
            {
                const auto *site = find_wiki_site(id.substr(6));
                if (site && route == "@article")
                {
                    if (auto article = wiktionary_.lookup(*site, query["word"]))
                        reply = {200, "text/html; charset=utf-8", wiki_page(*site, *article, theme)};
                    else
                        reply = {404, "text/html; charset=utf-8",
                                 "<!DOCTYPE html><meta charset=\"utf-8\"><p>" + html_escape(site->name) + " 没有这个词条。</p>"};
                }
            }
            else if (id == "@background")
            {
                // The interface background image (https://dict.dr-dict.local/@background/<name>).
                if (auto image = library_.background_image(route))
                    reply = {200, image->mime, std::move(image->data)};
            }
            else if (route == "@article")
            {
                // "script": s / t shows the article in simplified / traditional characters;
                // "variant" picks the traditional standard (t, tw, twp, hk).
                reply = {200, "text/html; charset=utf-8",
                         library_.article_page(id, query["word"], theme, query["script"], query["variant"])};
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

void Application::show_main()
{
    HWND main = window_.handle();
    if (!IsWindowVisible(main))
        ShowWindow(main, SW_SHOW); // keeps a maximized window maximized
    if (IsIconic(main))
        ShowWindow(main, SW_RESTORE);
    SetForegroundWindow(main);
}

void Application::quit()
{
    tray_.remove();
    DestroyWindow(window_.handle());
}

int Application::run(HINSTANCE instance)
{
    auto settings = library_.settings();
    auto shell = shell_override_.empty() ? settings.value("shell", std::string("mica")) : shell_override_;
    if (!window_.create(instance, shell == "direct2d" ? ShellStyle::Direct2D : ShellStyle::Mica, IDI_DR_DICT,
                        settings["window"].value("widthRatio", 0.4), settings["window"].value("heightRatio", 0.5),
                        theme_mode(settings.value("theme", std::string("system")))))
        return 1;
    window_.set_title(L"DR Dict");
    // Closing the window keeps DR Dict in the notification area (Ctrl+C+C and requests from other
    // programs keep working); only the tray menu's “退出” quits. Without a tray icon (no shell)
    // closing quits as before.
    tray_.menu = {{L"显示主窗口", L'', [this] { show_main(); }},
                  {},
                  {L"退出", L'', [this] { quit(); }}};
    tray_.palette = [this] {
        return TrayPalette{window_.resolved_theme()[0] == 'd', window_.background(), window_.text_color()};
    };
    tray_.on_activate = [this] { show_main(); };
    if (tray_.create(instance, IDI_DR_DICT, L"DR Dict"))
        window_.on_close = [this] { ShowWindow(window_.handle(), SW_HIDE); };
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
        // Client pixels → CSS pixels of the (possibly zoomed) page.
        const double scale = 96.0 / GetDpiForWindow(window_.handle()) / webview_.zoom();
        webview_.send({{"event", "caption-tooltip"},
                       {"text", narrow(text)},
                       {"left", button.left * scale},
                       {"width", (button.right - button.left) * scale}});
    };
    window_.set_theme(theme_mode(settings.value("theme", std::string("system"))));
    webview_.on_message = [this](const Json &request) { receive(request, webview_); };
    webview_.on_resource = [this](const std::wstring &uri, std::function<void(ResourceReply)> respond) {
        resource(uri, std::move(respond));
    };
    webview_.on_error = [this](const std::wstring &error) {
        MessageBoxW(window_.handle(), error.c_str(), L"DR Dict", MB_OK | MB_ICONERROR);
    };
    // Page zoom survives restarts; every change (Ctrl+wheel included) is saved and announced.
    webview_.set_zoom(settings.value("zoom", 1.0));
    webview_.on_zoom = [this](double factor) {
        webview_.send({{"event", "zoom"}, {"factor", factor}});
        queries_.enqueue([this, factor] { library_.save_settings({{"zoom", factor}}); });
    };
    webview_.resize(window_.content_bounds());
    webview_.create(window_.handle(), assets_, library_.root());
    // The popup is created hidden at start-up, so its WebView is ready (and the host TOPMOST)
    // before the first Ctrl+C+C.
    create_popup(instance, settings);
    apply_hotkey(settings);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Requests still in flight end at once, so the workers join without waiting for a server.
    http_cancel_->cancel();
    return static_cast<int>(msg.wParam);
}
} // namespace dr
