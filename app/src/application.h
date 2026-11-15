#pragma once
#include "dr/library.h"
#include "dr/window.h"
#include "agent_runner.h"
#include "copy_hotkey.h"
#include "http.h"
#include "popup.h"
#include "webview.h"
#include <condition_variable>
#include <optional>
#include <queue>
#include <thread>
namespace dr
{
// A small FIFO thread pool. Imports run on their own single worker so long indexing jobs never
// delay lookups, suggestions or article resources.
class Worker
{
  public:
    explicit Worker(size_t threads);
    ~Worker();
    void enqueue(std::function<void()> work);

  private:
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::queue<std::function<void()>> queue_;
    std::vector<std::jthread> threads_;
};

// One DR Dict per data folder: a hash of the folder's normalised path names the start-up mutex and
// tags the popup host (instance_property), so a second start finds the running instance.
inline size_t instance_tag(const fs::path &root)
{
    std::error_code error;
    auto path = fs::weakly_canonical(root, error);
    std::wstring text = (error ? fs::absolute(root) : path).lexically_normal().wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/'))
        text.pop_back();
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return std::hash<std::wstring>{}(text) | 1; // never 0, which GetPropW returns for "none"
}

// A word to show in the popup beside a screen point (from --lookup or another program).
struct LookupRequest
{
    std::string word;
    POINT at{};
};

class Application
{
  public:
    Application(fs::path root, fs::path assets, std::vector<fs::path> files, std::string shell,
                std::optional<LookupRequest> lookup = std::nullopt);
    int run(HINSTANCE instance);

  private:
    // Requests come from the main window's page or the popup's; answers go back to the sender.
    void receive(const Json &request, WebView &from);
    void reply(WebView &to, const Json &id, Json result);
    void reply_error(WebView &to, const Json &id, const std::string &error);
    // Ctrl+C+C lookup popup.
    void create_popup(HINSTANCE instance, const Json &settings);
    void apply_hotkey(const Json &settings);
    void copied_twice();
    // Lookups asked for by other programs; held until the popup is up and pinned.
    void request_lookup(LookupRequest request);
    void send_popup_lookup(const std::string &word);
    void resource(const std::wstring &uri, std::function<void(ResourceReply)> respond);
    std::vector<fs::path> pick_files();
    fs::path pick_folder();
    fs::path pick_image();
    void import(const Json &id, std::vector<fs::path> paths, bool rescan = false);
    void apply_settings(const Json &settings);
    // 小牛翻译 credentials: DPAPI-encrypted for this Windows user in <data>/niutrans-key.bin (as
    // DR Music keeps them) and never sent back to the page, which only sees niutrans_status().
    void load_niutrans_key();
    void save_niutrans_key(const std::string &app_id, const std::string &apikey);
    Json niutrans_status() const;
    // Online requests (translation, Wiktionary pages and their frames) run on online_, so a slow
    // network never holds up dictionary lookups on queries_.
    void online(const Json &id, const std::string &method, const Json &params, WebView &from);
    // AI 助查 through local agent CLIs (agent.*), as in DR Reader. Answers stream to the main
    // window as `agent` events keyed by the page's request id.
    bool receive_agent(const Json &id, const std::string &method, const Json &params, WebView &from);
    Window window_;
    WebView webview_;
    Popup popup_;
    WebView popup_view_;
    CopyHotkey hotkey_;
    POINT popup_cursor_{};
    std::optional<LookupRequest> pending_lookup_;
    Library library_;
    fs::path assets_;
    std::vector<fs::path> initial_files_;
    std::string shell_override_;
    std::shared_ptr<HttpCancel> http_cancel_ = std::make_shared<HttpCancel>();
    NiuTrans niutrans_;
    Wiktionary wiktionary_;
    // Workers come last: they are destroyed (joined) first, before anything their jobs use.
    Worker queries_{2};
    Worker imports_{1};
    Worker online_{3};
    // Created on first use; destroyed first, which stops running CLIs and waits for their threads
    // while the window they post to still exists.
    std::unique_ptr<AgentRunner> agents_;
};
} // namespace dr
