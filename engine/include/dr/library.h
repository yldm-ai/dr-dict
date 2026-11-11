#pragma once
#include "dr/chinese.h"
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
    /** Folder with OpenCC's configs and dictionaries; without it Chinese variant lookup is off. */
    void set_opencc_dir(const fs::path &dir);
    /** Whether OpenCC data was found, so the variant settings can work. */
    bool chinese_available();
    /** Other writings of `word` that lookups also search, per the current settings. */
    std::vector<std::string> chinese_variants(const std::string &word);

    Json list();
    Json settings();
    Json save_settings(const Json &patch);
    static Json default_settings();
    Json history();
    Json remember(const std::string &word);
    Json clear_history();
    Json forget_word(const std::string &word);
    // Starred words, newest first ([{word, at}]); toggling a starred word removes it.
    Json favorites();
    Json toggle_favorite(const std::string &word);

    // Files (.mdx/.ifo) or folders scanned recursively. Folders are remembered for rescan().
    Json add(const std::vector<fs::path> &paths, const Progress &progress);
    // Scans the remembered folders again and adds dictionaries that appeared in them. Removed
    // dictionaries stay removed; picking one's file again restores it.
    Json rescan(const Progress &progress);
    // Remembered folders: [{path, exists}]. Forgetting one keeps the dictionaries found in it.
    Json folders();
    Json remove_folder(const std::string &path);
    // Moves a dictionary to the "removed" group: listed with status "removed", disabled, its
    // index dropped. restore() indexes it again; forget() drops the entry once its file is gone.
    void remove(const std::string &id);
    Json restore(const std::string &id, const Progress &progress);
    Json forget(const std::string &id);
    Json update(const std::string &id, const Json &patch);
    Json reorder(const Json &ids);
    Json reindex(const std::string &id, const Progress &progress);
    Json info(const std::string &id);

    // `scope` limits a search to these dictionary ids (still only enabled ones); empty means all.
    Json suggest(const std::string &query, size_t limit, const std::vector<std::string> &scope = {});
    Json lookup(const std::string &word, const std::vector<std::string> &scope = {});
    // Complete HTML documents rendered inside the per-dictionary article frames.
    /**
     * `script`: "" as written, "s" simplified or "t" traditional (OpenCC, text only). `variant` is
     * the traditional standard (t, tw, twp, hk); empty or invalid uses the "traditionalVariant" setting.
     */
    std::string article_page(const std::string &id, const std::string &word, const std::string &theme,
                             const std::string &script = "", const std::string &variant = "");
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
    // sources.json: {"folders": [utf-8 paths]}
    void drop_index(const std::string &id);
    Json load_sources();
    void save_sources(const Json &sources);
    Json add_paths(const std::vector<fs::path> &paths, const Progress &progress, bool rescan);
    std::shared_ptr<Dictionary> open(const std::string &id);
    Json entry(const std::string &id);
    int number_for(sqlite3 *db, const std::string &id);
    Json build(const fs::path &path, const std::string &id, const Progress &progress);
    std::vector<std::pair<int, std::string>> enabled_numbers(const std::vector<std::string> &scope = {});
    Json status(const Json &meta);
    std::string page(const std::string &theme, const std::string &body);

    fs::path root_;
    std::mutex mutex_;    // dictionaries.json, settings, history and the open cache
    std::mutex db_mutex_; // the shared read connection
    sqlite3 *db_ = nullptr;
    std::map<std::string, std::shared_ptr<Dictionary>> open_;
    std::shared_ptr<ChineseVariants> chinese_;
};

std::string mime_type(const fs::path &path);
// Possible base forms for inflected English words ("studies" → "study"), tried after a miss.
std::vector<std::string> english_base_forms(const std::string &word);
} // namespace dr
