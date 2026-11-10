#pragma once
#include "dr/dictionary.h"
#include <map>
#include <mutex>
#include <optional>
struct sqlite3;
namespace dr
{
struct Resource
{
    std::string mime;
    std::string data;
};

// Owns the user's dictionary list, settings, history and the shared headword index.
// Dictionary files stay where the user keeps them; only the index lives in the data folder.
class Library
{
  public:
    using Progress = std::function<void(const Json &)>;
    explicit Library(fs::path root);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    const fs::path &root() const { return root_; }

    Json list();
    Json settings();
    Json save_settings(const Json &patch);
    static Json default_settings();
    Json history();
    Json remember(const std::string &word);
    Json clear_history();

    // Files (.mdx/.ifo) or folders scanned recursively.
    Json add(const std::vector<fs::path> &paths, const Progress &progress);
    void remove(const std::string &id);
    Json update(const std::string &id, const Json &patch);
    Json reorder(const Json &ids);
    Json reindex(const std::string &id, const Progress &progress);
    Json info(const std::string &id);

    Json suggest(const std::string &query, size_t limit);
    Json lookup(const std::string &word);
    // Complete HTML documents rendered inside the per-dictionary article frames.
    std::string article_page(const std::string &id, const std::string &word, const std::string &theme);
    std::string about_page(const std::string &id, const std::string &theme);
    std::optional<Resource> resource(const std::string &id, const std::string &path);
    std::optional<Resource> icon(const std::string &id);

    static constexpr int index_version = 1;

  private:
    struct Entry
    {
        Json meta;
        int number = 0;
    };
    Json load_entries();
    void save_entries(const Json &list);
    std::shared_ptr<Dictionary> open(const std::string &id);
    Json entry(const std::string &id);
    int number_for(sqlite3 *db, const std::string &id);
    Json build(const fs::path &path, const std::string &id, const Progress &progress);
    std::vector<std::pair<int, std::string>> enabled_numbers();
    Json status(const Json &meta);
    std::string page(const std::string &theme, const std::string &body);

    fs::path root_;
    std::mutex mutex_;    // dictionaries.json, settings, history and the open cache
    std::mutex db_mutex_; // the shared read connection
    sqlite3 *db_ = nullptr;
    std::map<std::string, std::shared_ptr<Dictionary>> open_;
};

std::string mime_type(const fs::path &path);
// Possible base forms for inflected English words ("studies" → "study"), tried after a miss.
std::vector<std::string> english_base_forms(const std::string &word);
} // namespace dr
