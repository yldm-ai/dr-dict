#pragma once
#include "dr/library.h"
#include "dr/window.h"
#include "webview.h"
#include <condition_variable>
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

class Application
{
  public:
    Application(fs::path root, fs::path assets, std::vector<fs::path> files, std::string shell);
    int run(HINSTANCE instance);

  private:
    void receive(const Json &request);
    void reply(const Json &id, Json result);
    void reply_error(const Json &id, const std::string &error);
    void resource(const std::wstring &uri, std::function<void(ResourceReply)> respond);
    std::vector<fs::path> pick_files();
    fs::path pick_folder();
    void import(const Json &id, std::vector<fs::path> paths);
    void apply_settings(const Json &settings);
    Window window_;
    WebView webview_;
    Library library_;
    fs::path assets_;
    std::vector<fs::path> initial_files_;
    std::string shell_override_;
    Worker queries_{2};
    Worker imports_{1};
};
} // namespace dr
