#include "dr/library.h"
#include "dr/text.h"
#include <algorithm>
#include <chrono>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
#include <unordered_set>
namespace dr
{
namespace
{
class Statement
{
  public:
    Statement(sqlite3 *db, const char *sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("索引数据库错误: ") + sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    Statement &bind(int i, int64_t v)
    {
        sqlite3_bind_int64(stmt_, i, v);
        return *this;
    }
    Statement &bind(int i, std::string_view v)
    {
        sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    bool step()
    {
        auto rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW)
            return true;
        if (rc != SQLITE_DONE)
            throw std::runtime_error(std::string("索引数据库错误: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
        return false;
    }
    void reset()
    {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    int64_t integer(int column) { return sqlite3_column_int64(stmt_, column); }
    std::string text(int column)
    {
        auto p = reinterpret_cast<const char *>(sqlite3_column_text(stmt_, column));
        return p ? std::string(p, sqlite3_column_bytes(stmt_, column)) : std::string();
    }

  private:
    sqlite3_stmt *stmt_ = nullptr;
};
void exec(sqlite3 *db, const char *sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK)
    {
        std::string message = error ? error : "unknown";
        sqlite3_free(error);
        throw std::runtime_error("索引数据库错误: " + message);
    }
}
sqlite3 *open_database(const fs::path &path)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(utf8(path).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        sqlite3_close(db);
        throw std::runtime_error("无法打开索引数据库");
    }
    sqlite3_busy_timeout(db, 15000);
    exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-32000;"
             "CREATE TABLE IF NOT EXISTS dicts(num INTEGER PRIMARY KEY, id TEXT UNIQUE NOT NULL);"
             "CREATE TABLE IF NOT EXISTS words(dict INTEGER NOT NULL, key TEXT NOT NULL, word TEXT NOT NULL,"
             " a INTEGER NOT NULL, b INTEGER NOT NULL);"
             "CREATE INDEX IF NOT EXISTS words_key ON words(key, dict);"
             "CREATE TABLE IF NOT EXISTS resources(dict INTEGER NOT NULL, key TEXT NOT NULL, volume INTEGER NOT NULL,"
             " a INTEGER NOT NULL, b INTEGER NOT NULL, PRIMARY KEY(dict, key)) WITHOUT ROWID;");
    return db;
}
struct Database
{
    sqlite3 *db;
    explicit Database(const fs::path &path) : db(open_database(path)) {}
    ~Database() { sqlite3_close(db); }
};
std::string make_id(const fs::path &path)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : lower_text(utf8(path)))
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[17];
    snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}
Json file_stamps(const std::vector<fs::path> &files)
{
    Json stamps = Json::array();
    for (auto &f : files)
    {
        std::error_code ec;
        auto size = fs::file_size(f, ec);
        auto time = fs::last_write_time(f, ec);
        stamps.push_back({{"path", utf8(f)},
                          {"size", ec ? 0 : size},
                          {"time", ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count())}});
    }
    return stamps;
}
int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
std::string clean_resource_path(std::string path)
{
    auto cut = path.find_first_of("?#");
    if (cut != std::string::npos)
        path.resize(cut);
    path = url_decode(path);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./"))
        path.erase(0, 2);
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    // Reject traversal outside the dictionary folder.
    size_t start = 0;
    while (start <= path.size())
    {
        auto end = path.find('/', start);
        auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..")
            return {};
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return path;
}
} // namespace

std::string mime_type(const fs::path &path)
{
    static const std::map<std::string, std::string> types = {
        {".css", "text/css"},          {".js", "text/javascript"},   {".html", "text/html"},
        {".htm", "text/html"},         {".png", "image/png"},        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},       {".gif", "image/gif"},        {".svg", "image/svg+xml"},
        {".webp", "image/webp"},       {".bmp", "image/bmp"},        {".ico", "image/x-icon"},
        {".mp3", "audio/mpeg"},        {".wav", "audio/wav"},        {".ogg", "audio/ogg"},
        {".oga", "audio/ogg"},         {".m4a", "audio/mp4"},        {".aac", "audio/aac"},
        {".opus", "audio/ogg"},        {".spx", "audio/ogg"},        {".mp4", "video/mp4"},
        {".webm", "video/webm"},       {".ttf", "font/ttf"},         {".otf", "font/otf"},
        {".woff", "font/woff"},        {".woff2", "font/woff2"},     {".json", "application/json"},
        {".txt", "text/plain"},        {".xml", "application/xml"},  {".pdf", "application/pdf"}};
    auto it = types.find(lower_ascii(path.extension().string()));
    return it == types.end() ? "application/octet-stream" : it->second;
}

std::vector<std::string> english_base_forms(const std::string &word)
{
    std::vector<std::string> out;
    auto w = lower_ascii(trim(word));
    if (w.size() < 3 || w.find_first_not_of("abcdefghijklmnopqrstuvwxyz'-") != std::string::npos)
        return out;
    auto add = [&](std::string s) {
        if (s.size() >= 2 && s != w && std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(std::move(s));
    };
    auto cut = [&](size_t n) { return w.substr(0, w.size() - n); };
    auto ends = [&](const char *s) { return w.ends_with(s); };
    auto undouble = [&](const std::string &s) {
        if (s.size() >= 3 && s[s.size() - 1] == s[s.size() - 2] && std::string("aeiou").find(s.back()) == std::string::npos)
            add(s.substr(0, s.size() - 1));
    };
    if (ends("'s"))
        add(cut(2));
    if (ends("ies") || ends("ied"))
        add(cut(3) + "y");
    if (ends("iest"))
        add(cut(4) + "y");
    if (ends("ier"))
        add(cut(3) + "y");
    if (ends("ves"))
    {
        add(cut(3) + "f");
        add(cut(3) + "fe");
    }
    if (ends("es"))
        add(cut(2));
    if (ends("s") && !ends("ss"))
        add(cut(1));
    if (ends("ed"))
    {
        add(cut(1));
        add(cut(2));
        undouble(cut(2));
    }
    if (ends("ing"))
    {
        add(cut(3));
        add(cut(3) + "e");
        undouble(cut(3));
    }
    if (ends("est"))
    {
        add(cut(3));
        add(cut(2));
        undouble(cut(3));
    }
    if (ends("er"))
    {
        add(cut(2));
        add(cut(1));
        undouble(cut(2));
    }
    if (ends("ly"))
        add(cut(2));
    return out;
}

Library::Library(fs::path root) : root_(std::move(root))
{
    fs::create_directories(root_);
    db_ = open_database(root_ / "index.db");
}
Library::~Library()
{
    sqlite3_close(db_);
}

Json Library::default_settings()
{
    return {{"theme", "system"},
            {"shell", "mica"},
            {"invertArticles", true},
            {"articleWidth", "comfortable"},
            // Look Chinese queries up in their simplified and traditional writings as well.
            {"chineseConversion", false},
            {"chineseVariant", "t"},
            // Initial window size as a fraction of the current monitor (width 2/5, height 1/2).
            {"window", {{"widthRatio", 0.4}, {"heightRatio", 0.5}}}};
}
void Library::set_opencc_dir(const fs::path &dir)
{
    std::lock_guard lock(mutex_);
    chinese_ = std::make_shared<ChineseVariants>(dir);
}
bool Library::chinese_available()
{
    std::lock_guard lock(mutex_);
    return chinese_ && chinese_->available();
}
std::vector<std::string> Library::chinese_variants(const std::string &word)
{
    std::shared_ptr<ChineseVariants> chinese;
    {
        std::lock_guard lock(mutex_);
        chinese = chinese_;
    }
    if (!chinese || !has_han(word))
        return {};
    auto current = settings();
    if (!current.value("chineseConversion", false))
        return {};
    return chinese->expand(word, current.value("chineseVariant", std::string("t")));
}
Json Library::settings()
{
    std::lock_guard lock(mutex_);
    auto result = default_settings();
    try
    {
        auto path = root_ / "settings.json";
        if (fs::exists(path))
        {
            auto saved = Json::parse(read_file(path));
            // The window size is not user-configurable; older files must not pin a stale size.
            for (auto &[key, value] : saved.items())
                if (key != "window" && result.contains(key) && result[key].type() == value.type())
                    result[key] = value;
        }
    }
    catch (const std::exception &)
    {
    }
    return result;
}
Json Library::save_settings(const Json &patch)
{
    auto current = settings();
    static const std::set<std::string> themes = {"light", "dark", "system"}, shells = {"mica", "direct2d"},
                                        widths = {"narrow", "comfortable", "wide"};
    if (patch.contains("theme") && patch["theme"].is_string() && themes.count(patch["theme"]))
        current["theme"] = patch["theme"];
    if (patch.contains("shell") && patch["shell"].is_string() && shells.count(patch["shell"]))
        current["shell"] = patch["shell"];
    if (patch.contains("articleWidth") && patch["articleWidth"].is_string() && widths.count(patch["articleWidth"]))
        current["articleWidth"] = patch["articleWidth"];
    if (patch.contains("invertArticles") && patch["invertArticles"].is_boolean())
        current["invertArticles"] = patch["invertArticles"];
    if (patch.contains("chineseConversion") && patch["chineseConversion"].is_boolean())
        current["chineseConversion"] = patch["chineseConversion"];
    if (patch.contains("chineseVariant") && patch["chineseVariant"].is_string() &&
        valid_chinese_variant(patch["chineseVariant"]))
        current["chineseVariant"] = patch["chineseVariant"];
    std::lock_guard lock(mutex_);
    write_atomic(root_ / "settings.json", current.dump(2));
    return current;
}

Json Library::history()
{
    std::lock_guard lock(mutex_);
    try
    {
        auto path = root_ / "history.json";
        if (fs::exists(path))
        {
            auto list = Json::parse(read_file(path));
            if (list.is_array())
                return list;
        }
    }
    catch (const std::exception &)
    {
    }
    return Json::array();
}
Json Library::remember(const std::string &word)
{
    auto w = trim(word);
    auto list = history();
    if (w.empty() || w.size() > 200)
        return list;
    Json next = Json::array({{{"word", w}, {"at", now_ms()}}});
    for (auto &item : list)
        if (item.is_object() && item.value("word", "") != w && next.size() < 200)
            next.push_back(item);
    std::lock_guard lock(mutex_);
    write_atomic(root_ / "history.json", next.dump());
    return next;
}
Json Library::clear_history()
{
    std::lock_guard lock(mutex_);
    write_atomic(root_ / "history.json", "[]");
    return Json::array();
}

Json Library::load_entries()
{
    auto path = root_ / "dictionaries.json";
    if (!fs::exists(path))
        return Json::array();
    try
    {
        auto data = Json::parse(read_file(path));
        if (data.is_object() && data.contains("dictionaries") && data["dictionaries"].is_array())
            return data["dictionaries"];
    }
    catch (const std::exception &)
    {
    }
    return Json::array();
}
void Library::save_entries(const Json &list)
{
    write_atomic(root_ / "dictionaries.json", Json{{"version", 1}, {"dictionaries", list}}.dump(2));
}
Json Library::load_sources()
{
    // Removed dictionaries are kept as entries marked "removed" in dictionaries.json; an older
    // "excluded" id list here is ignored and dropped on the next save.
    Json sources = {{"folders", Json::array()}};
    try
    {
        auto path = root_ / "sources.json";
        if (fs::exists(path))
        {
            auto data = Json::parse(read_file(path));
            if (data.contains("folders") && data["folders"].is_array())
                sources["folders"] = data["folders"];
        }
    }
    catch (const std::exception &)
    {
    }
    return sources;
}
void Library::save_sources(const Json &sources)
{
    write_atomic(root_ / "sources.json", sources.dump(2));
}
Json Library::status(const Json &meta)
{
    Json item = meta;
    item.erase("files");
    std::error_code ec;
    std::string state = "ready";
    const bool exists = fs::exists(path8(meta.value("path", "")), ec);
    // A removed dictionary has no index; only whether its file still exists matters.
    item["removed"] = meta.value("removed", false);
    if (item["removed"] == true)
    {
        item["fileMissing"] = !exists;
        item["status"] = "removed";
        return item;
    }
    if (!exists)
        state = "missing";
    else if (meta.value("indexVersion", 0) != index_version)
        state = "stale";
    else if (meta.contains("files"))
        for (auto &f : meta["files"])
        {
            auto p = path8(f.value("path", ""));
            auto size = fs::file_size(p, ec);
            if (ec || size != f.value("size", uint64_t(0)))
            {
                state = "stale";
                break;
            }
        }
    item["status"] = state;
    return item;
}
Json Library::list()
{
    std::lock_guard lock(mutex_);
    Json result = Json::array();
    for (auto &meta : load_entries())
        result.push_back(status(meta));
    return result;
}
Json Library::entry(const std::string &id)
{
    for (auto &meta : load_entries())
        if (meta.value("id", "") == id)
            return meta;
    throw std::runtime_error("找不到该词典");
}
std::shared_ptr<Dictionary> Library::open(const std::string &id)
{
    std::lock_guard lock(mutex_);
    if (auto it = open_.find(id); it != open_.end())
        return it->second;
    auto meta = entry(id);
    std::shared_ptr<Dictionary> dict = open_dictionary(path8(meta.value("path", "")));
    open_[id] = dict;
    return dict;
}
int Library::number_for(sqlite3 *db, const std::string &id)
{
    {
        Statement find(db, "SELECT num FROM dicts WHERE id=?");
        find.bind(1, id);
        if (find.step())
            return static_cast<int>(find.integer(0));
    }
    Statement insert(db, "INSERT INTO dicts(id) VALUES(?)");
    insert.bind(1, id);
    insert.step();
    return static_cast<int>(sqlite3_last_insert_rowid(db));
}
std::vector<std::pair<int, std::string>> Library::enabled_numbers()
{
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mutex_);
        for (auto &meta : load_entries())
            if (meta.value("enabled", true) && !meta.value("removed", false))
                ids.push_back(meta.value("id", ""));
    }
    std::vector<std::pair<int, std::string>> result;
    std::lock_guard lock(db_mutex_);
    Statement find(db_, "SELECT num FROM dicts WHERE id=?");
    for (auto &id : ids)
    {
        find.reset();
        find.bind(1, id);
        if (find.step())
            result.emplace_back(static_cast<int>(find.integer(0)), id);
    }
    return result;
}

Json Library::build(const fs::path &path, const std::string &id, const Progress &progress)
{
    auto dict = open_dictionary(path);
    const auto name = dict->title();
    Database database(root_ / "index.db");
    auto *db = database.db;
    exec(db, "BEGIN IMMEDIATE");
    try
    {
        int num = number_for(db, id);
        {
            Statement clear_words(db, "DELETE FROM words WHERE dict=?");
            clear_words.bind(1, num).step();
            Statement clear_resources(db, "DELETE FROM resources WHERE dict=?");
            clear_resources.bind(1, num).step();
        }
        Statement word(db, "INSERT INTO words(dict, key, word, a, b) VALUES(?,?,?,?,?)");
        Statement resource(db, "INSERT OR IGNORE INTO resources(dict, key, volume, a, b) VALUES(?,?,?,?,?)");
        uint64_t words = 0, resources = 0;
        double last = -1;
        IndexSink sink;
        sink.word = [&](std::string_view w, uint64_t a, uint64_t b) {
            auto key = fold_key(w);
            if (key.empty())
                return;
            word.reset();
            word.bind(1, num).bind(2, key).bind(3, w).bind(4, static_cast<int64_t>(a)).bind(5, static_cast<int64_t>(b));
            word.step();
            ++words;
        };
        sink.resource = [&](std::string_view p, int volume, uint64_t a, uint64_t b) {
            resource.reset();
            resource.bind(1, num).bind(2, lower_text(p)).bind(3, volume).bind(4, static_cast<int64_t>(a))
                .bind(5, static_cast<int64_t>(b));
            resource.step();
            ++resources;
        };
        sink.progress = [&](double fraction) {
            if (progress && (fraction - last >= 0.01 || fraction >= 1.0))
            {
                last = fraction;
                progress({{"name", name}, {"fraction", fraction}});
            }
        };
        dict->index(sink);
        exec(db, "COMMIT");
        Json meta = {{"id", id},
                     {"path", utf8(path)},
                     {"format", dict->format()},
                     {"name", name},
                     {"enabled", true},
                     {"entries", words},
                     {"resources", resources},
                     {"hasIcon", dict->icon().has_value()},
                     {"indexVersion", index_version},
                     {"indexedAt", now_ms()},
                     {"files", file_stamps(dict->files())}};
        return meta;
    }
    catch (...)
    {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

namespace
{
fs::path full_path(const fs::path &path)
{
    std::error_code ec;
    auto result = fs::weakly_canonical(fs::absolute(path), ec);
    return ec ? fs::absolute(path) : result;
}
// Whether `child` is `parent` or lies inside it (Windows paths compare case-insensitively).
bool within(const fs::path &child, const fs::path &parent)
{
    auto c = lower_text(utf8(child.lexically_normal())), p = lower_text(utf8(parent.lexically_normal()));
    std::replace(c.begin(), c.end(), '\\', '/');
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return c == p || (c.starts_with(p) && c[p.size()] == '/');
}
} // namespace

Json Library::add(const std::vector<fs::path> &paths, const Progress &progress)
{
    return add_paths(paths, progress, false);
}

Json Library::rescan(const Progress &progress)
{
    std::vector<fs::path> existing;
    {
        std::lock_guard lock(mutex_);
        // Keep the document alive: a range-for over load_sources()["folders"] would iterate a
        // member of a temporary that is already destroyed (C++20 range-for lifetime rules).
        const auto sources = load_sources();
        for (auto &folder : sources["folders"])
        {
            std::error_code ec;
            auto path = path8(folder.get<std::string>());
            if (fs::is_directory(path, ec))
                existing.push_back(path);
        }
    }
    auto result = existing.empty()
                      ? Json{{"dictionaries", list()}, {"added", 0}, {"skipped", 0}, {"errors", Json::array()}}
                      : add_paths(existing, progress, true);
    result["folders"] = existing.size();
    return result;
}

Json Library::folders()
{
    std::lock_guard lock(mutex_);
    Json result = Json::array();
    const auto sources = load_sources();
    for (auto &folder : sources["folders"])
    {
        std::error_code ec;
        result.push_back({{"path", folder}, {"exists", fs::is_directory(path8(folder.get<std::string>()), ec)}});
    }
    return result;
}

Json Library::remove_folder(const std::string &path)
{
    {
        std::lock_guard lock(mutex_);
        auto sources = load_sources();
        Json next = Json::array();
        for (auto &folder : sources["folders"])
            if (folder.get<std::string>() != path)
                next.push_back(folder);
        sources["folders"] = next;
        save_sources(sources);
    }
    return folders();
}

Json Library::add_paths(const std::vector<fs::path> &paths, const Progress &progress, bool rescan)
{
    // Each candidate remembers whether the user picked it directly or it was found in a folder.
    std::vector<std::pair<fs::path, bool>> candidates;
    Json errors = Json::array();
    std::vector<fs::path> picked_folders;
    for (auto &input : paths)
    {
        std::error_code ec;
        if (fs::is_directory(input, ec))
        {
            picked_folders.push_back(full_path(input));
            for (fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_regular_file(ec) && is_dictionary_file(it->path()))
                    candidates.emplace_back(it->path(), true);
            }
        }
        else if (is_dictionary_file(input))
            candidates.emplace_back(input, false);
        else
            errors.push_back(utf8(input.filename()) + ": 请选择 .mdx 或 .ifo 文件");
    }
    std::sort(candidates.begin(), candidates.end());
    if (!rescan)
    {
        std::lock_guard lock(mutex_);
        auto sources = load_sources();
        {
            // Remember new folders; one inside a remembered folder is already covered, and one
            // that contains remembered folders replaces them.
            for (auto &folder : picked_folders)
            {
                Json kept = Json::array();
                bool covered = false;
                for (auto &known : sources["folders"])
                {
                    auto known_path = path8(known.get<std::string>());
                    if (within(folder, known_path))
                        covered = true;
                    if (covered || !within(known_path, folder))
                        kept.push_back(known);
                }
                if (!covered)
                    kept.push_back(utf8(folder));
                sources["folders"] = kept;
            }
            save_sources(sources);
        }
    }
    int added = 0, skipped = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        auto path = full_path(candidates[i].first);
        auto id = make_id(path);
        bool removed = false, known = false;
        {
            std::lock_guard lock(mutex_);
            for (auto &m : load_entries())
                if (m.value("id", "") == id)
                    known = true, removed = m.value("removed", false);
        }
        if (known)
        {
            // A removed dictionary stays in the "removed" group when its folder is scanned; picking
            // its file directly restores it.
            if (removed && !rescan && !candidates[i].second)
            {
                try
                {
                    restore(id, progress);
                    ++added;
                }
                catch (const std::exception &e)
                {
                    errors.push_back(utf8(path.filename()) + ": " + e.what());
                }
            }
            else
                ++skipped;
            continue;
        }
        try
        {
            auto meta = build(path, id, [&](const Json &p) {
                if (progress)
                {
                    auto event = p;
                    event["index"] = i;
                    event["count"] = candidates.size();
                    progress(event);
                }
            });
            std::lock_guard lock(mutex_);
            auto list = load_entries();
            list.push_back(meta);
            save_entries(list);
            ++added;
        }
        catch (const std::exception &e)
        {
            errors.push_back(utf8(path.filename()) + ": " + e.what());
        }
    }
    if (candidates.empty() && errors.empty() && !rescan)
        errors.push_back("所选位置没有找到 .mdx 或 .ifo 词典");
    return {{"dictionaries", list()}, {"added", added}, {"skipped", skipped}, {"errors", errors}};
}

void Library::remove(const std::string &id)
{
    {
        // The entry stays, marked removed, so the library can list it and folder scans skip it;
        // only its index is dropped.
        std::lock_guard lock(mutex_);
        auto list = load_entries();
        for (auto &m : list)
            if (m.value("id", "") == id)
            {
                m["removed"] = true;
                m["enabled"] = false;
            }
        save_entries(list);
        open_.erase(id);
    }
    drop_index(id);
}

Json Library::restore(const std::string &id, const Progress &progress)
{
    Json old;
    {
        std::lock_guard lock(mutex_);
        old = entry(id);
        open_.erase(id);
    }
    auto meta = build(path8(old.value("path", "")), id, progress);
    std::lock_guard lock(mutex_);
    auto list = load_entries();
    for (auto &m : list)
        if (m.value("id", "") == id)
            m = meta; // enabled, no longer removed
    save_entries(list);
    Json result = Json::array();
    for (auto &item : list)
        result.push_back(status(item));
    return result;
}

Json Library::forget(const std::string &id)
{
    std::lock_guard lock(mutex_);
    auto list = load_entries();
    Json next = Json::array();
    for (auto &m : list)
    {
        if (m.value("id", "") != id)
            next.push_back(m);
        // The removed entry is what keeps folder scans from adding the file again, so it may
        // only be forgotten once the file itself is gone.
        else
        {
            std::error_code ec;
            if (!m.value("removed", false) || fs::exists(path8(m.value("path", "")), ec))
                throw std::runtime_error("只能清除文件已不存在的已移除词典");
        }
    }
    save_entries(next);
    Json result = Json::array();
    for (auto &item : next)
        result.push_back(status(item));
    return result;
}

void Library::drop_index(const std::string &id)
{
    Database database(root_ / "index.db");
    exec(database.db, "BEGIN IMMEDIATE");
    {
        Statement find(database.db, "SELECT num FROM dicts WHERE id=?");
        find.bind(1, id);
        if (find.step())
        {
            auto num = find.integer(0);
            Statement w(database.db, "DELETE FROM words WHERE dict=?");
            w.bind(1, num).step();
            Statement r(database.db, "DELETE FROM resources WHERE dict=?");
            r.bind(1, num).step();
            Statement d(database.db, "DELETE FROM dicts WHERE num=?");
            d.bind(1, num).step();
        }
    }
    exec(database.db, "COMMIT");
}

Json Library::update(const std::string &id, const Json &patch)
{
    std::lock_guard lock(mutex_);
    auto list = load_entries();
    for (auto &m : list)
        if (m.value("id", "") == id)
        {
            if (patch.contains("enabled") && patch["enabled"].is_boolean())
            {
                if (m.value("removed", false) && patch["enabled"] == true)
                    throw std::runtime_error("该词典已移除，请先恢复");
                m["enabled"] = patch["enabled"];
            }
            if (patch.contains("name") && patch["name"].is_string())
            {
                auto name = trim(patch["name"].get<std::string>());
                if (!name.empty() && name.size() < 300)
                    m["name"] = name;
            }
        }
    save_entries(list);
    Json result = Json::array();
    for (auto &meta : list)
        result.push_back(status(meta));
    return result;
}

Json Library::reorder(const Json &ids)
{
    std::lock_guard lock(mutex_);
    auto list = load_entries();
    Json next = Json::array();
    std::set<std::string> placed;
    if (ids.is_array())
        for (auto &id : ids)
            for (auto &m : list)
                if (id.is_string() && m.value("id", "") == id.get<std::string>() && placed.insert(id).second)
                    next.push_back(m);
    for (auto &m : list)
        if (!placed.count(m.value("id", "")))
            next.push_back(m);
    save_entries(next);
    Json result = Json::array();
    for (auto &meta : next)
        result.push_back(status(meta));
    return result;
}

Json Library::reindex(const std::string &id, const Progress &progress)
{
    Json old;
    {
        std::lock_guard lock(mutex_);
        old = entry(id);
        if (old.value("removed", false))
            throw std::runtime_error("该词典已移除，请先恢复");
        open_.erase(id);
    }
    auto meta = build(path8(old.value("path", "")), id, progress);
    meta["enabled"] = old.value("enabled", true);
    if (old.contains("name"))
        meta["name"] = old["name"];
    std::lock_guard lock(mutex_);
    auto list = load_entries();
    for (auto &m : list)
        if (m.value("id", "") == id)
            m = meta;
    save_entries(list);
    Json result = Json::array();
    for (auto &item : list)
        result.push_back(status(item));
    return result;
}

Json Library::info(const std::string &id)
{
    Json meta;
    {
        std::lock_guard lock(mutex_);
        meta = entry(id);
    }
    auto result = status(meta);
    Json files = Json::array();
    if (meta.contains("files"))
        for (auto &f : meta["files"])
            files.push_back({{"path", f.value("path", "")}, {"size", f.value("size", uint64_t(0))}});
    result["files"] = files;
    return result;
}

Json Library::suggest(const std::string &query, size_t limit)
{
    Json result = Json::array();
    std::vector<std::string> keys;
    for (auto &w : [&] {
             std::vector<std::string> all = {query};
             for (auto &v : chinese_variants(query))
                 all.push_back(v);
             return all;
         }())
        if (auto key = fold_key(w); !key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    if (keys.empty())
        return result;
    std::unordered_set<int> enabled;
    for (auto &[num, id] : enabled_numbers())
        enabled.insert(num);
    if (enabled.empty())
        return result;
    std::lock_guard lock(db_mutex_);
    // The typed writing comes first; its simplified/traditional variants fill the rest.
    std::unordered_set<std::string> seen;
    for (auto &key : keys)
    {
        Statement s(db_, "SELECT key, word, dict FROM words WHERE key >= ?1 AND key < ?2 ORDER BY key LIMIT ?3");
        s.bind(1, key).bind(2, key + "\xf4\x8f\xbf\xbf").bind(3, static_cast<int64_t>(limit * 8));
        while (s.step() && result.size() < limit)
        {
            if (!enabled.count(static_cast<int>(s.integer(2))))
                continue;
            if (!seen.insert(s.text(0)).second)
                continue;
            result.push_back(s.text(1));
        }
        if (result.size() >= limit)
            break;
    }
    return result;
}

Json Library::lookup(const std::string &input)
{
    auto word = trim(input);
    auto order = enabled_numbers();
    std::map<std::string, std::string> names;
    {
        std::lock_guard lock(mutex_);
        for (auto &m : load_entries())
            names[m.value("id", "")] = m.value("name", "");
    }
    // Counts entries per dictionary over several writings of the word; `matched` collects the
    // writings that were found at all.
    auto find = [&](const std::vector<std::string> &writings, std::vector<std::string> *matched) {
        Json hits = Json::array();
        std::map<int, int64_t> counts;
        std::vector<std::string> keys;
        {
            std::lock_guard lock(db_mutex_);
            for (auto &w : writings)
            {
                auto key = fold_key(w);
                if (key.empty() || std::find(keys.begin(), keys.end(), key) != keys.end())
                    continue;
                keys.push_back(key);
                Statement s(db_, "SELECT dict, COUNT(*) FROM words WHERE key=? GROUP BY dict");
                s.bind(1, key);
                bool any = false;
                while (s.step())
                {
                    counts[static_cast<int>(s.integer(0))] += s.integer(1);
                    any = true;
                }
                if (any && matched)
                    matched->push_back(w);
            }
        }
        for (auto &[num, id] : order)
            if (auto it = counts.find(num); it != counts.end())
                hits.push_back({{"id", id}, {"name", names[id]}, {"count", it->second}});
        return hits;
    };
    auto variants = chinese_variants(word);
    std::vector<std::string> writings = {word}, matched;
    writings.insert(writings.end(), variants.begin(), variants.end());
    Json result = {{"word", word}, {"query", word}, {"results", find(writings, &matched)}};
    // Simplified/traditional writings that also matched, for the result header.
    Json also = Json::array();
    for (auto &m : matched)
        if (m != word)
            also.push_back(m);
    if (!also.empty())
        result["variants"] = also;
    if (result["results"].empty())
        for (auto &base : english_base_forms(word))
        {
            auto hits = find({base}, nullptr);
            if (!hits.empty())
            {
                result["word"] = base;
                result["results"] = hits;
                break;
            }
        }
    if (result["results"].empty())
        result["suggestions"] = suggest(word.substr(0, std::max<size_t>(1, word.size() > 3 ? word.size() - 1 : word.size())), 12);
    return result;
}

std::string Library::page(const std::string &theme, const std::string &body)
{
    return "<!DOCTYPE html><html data-dr-theme=\"" + html_escape(theme) +
           "\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<link rel=\"stylesheet\" href=\"https://app.dr-dict.local/frame.css\">"
           "<script src=\"https://app.dr-dict.local/frame.js\"></script></head><body>" +
           body + "</body></html>";
}

std::string Library::article_page(const std::string &id, const std::string &word, const std::string &theme)
{
    auto dict = open(id);
    int num = 0;
    std::vector<std::pair<uint64_t, uint64_t>> queue;
    auto collect = [&](const std::string &w) {
        std::lock_guard lock(db_mutex_);
        if (!num)
        {
            Statement n(db_, "SELECT num FROM dicts WHERE id=?");
            n.bind(1, id);
            if (!n.step())
                return;
            num = static_cast<int>(n.integer(0));
        }
        Statement s(db_, "SELECT a, b FROM words WHERE key=? AND dict=? ORDER BY rowid");
        s.bind(1, fold_key(w)).bind(2, num);
        while (s.step())
            queue.emplace_back(static_cast<uint64_t>(s.integer(0)), static_cast<uint64_t>(s.integer(1)));
    };
    collect(word);
    // The typed writing's articles come first, then those found under its other writings.
    for (auto &variant : chinese_variants(word))
        if (fold_key(variant) != fold_key(word))
            collect(variant);
    std::set<uint64_t> seen;
    std::string body;
    int articles = 0;
    // Reference: GoldenDict-ng mdx.cc MdxArticleRequest::run follows "@@@LINK=" by queueing the
    // target's articles; here the hop count is bounded so redirect cycles terminate.
    for (size_t i = 0; i < queue.size() && i < 64; ++i)
    {
        auto [a, b] = queue[i];
        if (!seen.insert(a).second)
            continue;
        auto text = dict->article(a, b);
        if (text.starts_with("@@@LINK="))
        {
            auto target = trim(text.substr(8));
            auto newline = target.find_first_of("\r\n");
            if (newline != std::string::npos)
                target = trim(target.substr(0, newline));
            collect(target);
            continue;
        }
        if (articles++)
            body += "<hr class=\"dr-separator\">";
        body += "<div class=\"dr-entry\">" + text + "</div>";
    }
    if (!articles)
        body = "<p class=\"dr-empty\">该词典中没有这个词条的正文。</p>";
    return page(theme, body);
}

std::string Library::about_page(const std::string &id, const std::string &theme)
{
    auto dict = open(id);
    auto description = dict->description();
    if (trim(description).empty())
        description = "<p class=\"dr-empty\">该词典没有提供简介。</p>";
    return page(theme, "<div class=\"dr-about\">" + description + "</div>");
}

std::optional<Resource> Library::resource(const std::string &id, const std::string &raw_path)
{
    auto path = clean_resource_path(raw_path);
    if (path.empty())
        return std::nullopt;
    auto dict = open(id);
    // Reference: GoldenDict-ng MdxDictionary::loadResourceFile lets a file beside the dictionary
    // override the same name inside the .mdd volumes.
    std::error_code ec;
    auto folder = dict->resource_folder();
    auto local = folder / path8(path);
    if (fs::is_regular_file(local, ec))
        return Resource{mime_type(local), read_file(local)};
    int num = 0;
    for (int hop = 0; hop < 4; ++hop)
    {
        int volume = 0;
        uint64_t a = 0, b = 0;
        {
            std::lock_guard lock(db_mutex_);
            if (!num)
            {
                Statement n(db_, "SELECT num FROM dicts WHERE id=?");
                n.bind(1, id);
                if (!n.step())
                    return std::nullopt;
                num = static_cast<int>(n.integer(0));
            }
            Statement s(db_, "SELECT volume, a, b FROM resources WHERE dict=? AND key=?");
            s.bind(1, num).bind(2, lower_text(path));
            if (!s.step())
                return std::nullopt;
            volume = static_cast<int>(s.integer(0));
            a = static_cast<uint64_t>(s.integer(1));
            b = static_cast<uint64_t>(s.integer(2));
        }
        auto data = dict->resource(volume, a, b);
        // Resource redirects are stored as UTF-16LE "@@@LINK=target".
        static const std::string link("@\0@\0@\0L\0I\0N\0K\0=\0", 16);
        if (data.size() > link.size() && data.compare(0, link.size(), link) == 0)
        {
            path = clean_resource_path(trim(to_utf8(data.substr(link.size()), "UTF-16LE")));
            continue;
        }
        return Resource{mime_type(path8(path)), std::move(data)};
    }
    return std::nullopt;
}

std::optional<Resource> Library::icon(const std::string &id)
{
    auto dict = open(id);
    auto path = dict->icon();
    if (!path)
        return std::nullopt;
    return Resource{mime_type(*path), read_file(*path)};
}
} // namespace dr
