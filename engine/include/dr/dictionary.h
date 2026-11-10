#pragma once
#include "dr/paths.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace dr
{
// Receives headwords and resources while a dictionary is indexed. `a` and `b` are opaque
// format-specific locators (MDict: record offset/size; StarDict: data offset/size).
struct IndexSink
{
    std::function<void(std::string_view word, uint64_t a, uint64_t b)> word;
    std::function<void(std::string_view path, int volume, uint64_t a, uint64_t b)> resource;
    std::function<void(double fraction)> progress;
};

class Dictionary
{
  public:
    virtual ~Dictionary() = default;
    virtual std::string format() const = 0;
    virtual std::string title() const = 0;
    // HTML description shown on the dictionary's information page.
    virtual std::string description() const = 0;
    virtual void index(const IndexSink &sink) = 0;
    // Article text converted to UTF-8 HTML. MDict "@@@LINK=" redirects are returned verbatim.
    virtual std::string article(uint64_t a, uint64_t b) = 0;
    // Bytes of an indexed resource (MDict .mdd volumes).
    virtual std::string resource(int volume, uint64_t a, uint64_t b)
    {
        (void)volume, (void)a, (void)b;
        return {};
    }
    // Folder whose files are served next to the dictionary's articles.
    virtual fs::path resource_folder() const = 0;
    virtual std::optional<fs::path> icon() const = 0;
    // All files that make up the dictionary; their sizes/times detect changes.
    virtual std::vector<fs::path> files() const = 0;
};

// Opens a dictionary by its main file (.mdx or .ifo). Throws for unsupported files.
std::unique_ptr<Dictionary> open_dictionary(const fs::path &main_file);
bool is_dictionary_file(const fs::path &path);
} // namespace dr
