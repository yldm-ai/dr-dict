#pragma once
// Online sources: 小牛翻译 (NiuTrans) sentence translation and Wiktionary articles. The network
// itself is injected (HttpFetch), so the request building, reply parsing and HTML rewriting here
// run in tests without it; the app supplies a WinHTTP client limited to the hosts below.
#include "dr/paths.h"
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace dr
{
struct HttpRequest
{
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    size_t limit = 8 * 1024 * 1024; // response bytes
    unsigned timeout_ms = 10000;    // receive timeout
};
struct HttpResponse
{
    int status = 0;
    std::string body;
};
/** Performs a request; throws on transport failure, returns any HTTP status. */
using HttpFetch = std::function<HttpResponse(const HttpRequest &)>;

/** Lowercase hex MD5 (Windows CNG). */
std::string md5_hex(std::string_view data);

/** Unicode code points in UTF-8 text (invalid bytes count one each). */
size_t code_points(std::string_view text);

// 小牛翻译 here only translates between Chinese and English: text that is mostly Han characters
// goes zh → en, anything else en → zh. A Han character counts as much as three Latin letters, so
// "我喜欢 Python 编程" is Chinese and "I visited 北京 last year" is English.
struct Direction
{
    std::string from, to;
};
Direction translation_direction(std::string_view text);

/**
 * 小牛翻译 text API v2 (https://niutrans.com/documents/contents/transapi_text_v2), the protocol
 * DR Music's native/niutrans.cpp uses: POST https://api.niutrans.com/v2/text/translate with JSON
 * {from, to, srcText, appId, timestamp, authStr}, where authStr is the MD5 of the "key=value" pairs
 * (apikey included, authStr and empty values left out) joined by "&" in ASCII key order.
 */
class NiuTrans
{
  public:
    static constexpr const char *host = "api.niutrans.com";
    static constexpr size_t max_length = 5000; // characters per request (API limit)
    explicit NiuTrans(HttpFetch fetch, std::function<long long()> clock = {});
    void set_credentials(std::string app_id, std::string apikey);
    bool configured() const;
    std::string app_id() const;
    static std::string auth_str(const std::map<std::string, std::string> &params, const std::string &apikey);
    /**
     * {text, from, to, translation}. Results are kept in memory per text, so showing a sentence
     * again (back, forward, a theme change) costs no characters; `cached` = false asks again.
     * Throws std::runtime_error with a message for the user.
     */
    Json translate(const std::string &text, bool cached = true);

  private:
    HttpFetch fetch_;
    std::function<long long()> clock_;
    mutable std::mutex mutex_;
    std::string app_id_, apikey_;
    std::chrono::steady_clock::time_point last_request_{};
    std::map<std::string, std::string> cache_; // from>to\ntext -> translation
    std::vector<std::string> cache_order_;
};

// A Wiktionary edition. `languages` are the level-2 section ids kept from a page (a page holds
// every language's entry for the word); a page with none of them is shown whole.
struct WikiSite
{
    std::string id;   // "en", "zh"
    std::string name; // shown as the dictionary name
    std::string host; // "en.wiktionary.org"
    std::string variant;
    std::vector<std::string> languages;
};
const std::vector<WikiSite> &wiki_sites();
const WikiSite *find_wiki_site(std::string_view id);

struct WikiArticle
{
    std::string title; // the page's own title, after redirects
    std::string html;  // rewritten article body
};
/** action=parse URL for `page`, with redirects followed and edit links and the ToC left out. */
std::string wiki_parse_url(const WikiSite &site, std::string_view page);
/** The article in an action=parse reply (formatversion=2); nullopt for a missing page or an error. */
std::optional<WikiArticle> parse_wiki_reply(const WikiSite &site, std::string_view body);
/** Keeps the preamble and the level-2 sections whose heading id is listed; unchanged when none is. */
std::string keep_wiki_languages(std::string_view html, const std::vector<std::string> &languages);
/**
 * Makes parser output work inside an article frame: scripts are dropped, links to other entries
 * become entry:// lookups, other wiki pages and protocol-relative or site-relative URLs become
 * absolute https links (opened outside, or loaded from *.wikimedia.org).
 */
std::string rewrite_wiki_html(const WikiSite &site, std::string_view html);
/** The complete frame document: CSP, the shared frame.css/frame.js, wiki.css/wiki.js and a source line. */
std::string wiki_page(const WikiSite &site, const WikiArticle &article, const std::string &theme);

/** Looks words up in Wiktionary editions, remembering recent articles and misses. */
class Wiktionary
{
  public:
    explicit Wiktionary(HttpFetch fetch);
    /** The article for `word` (case variants tried after a miss); nullopt when there is none. */
    std::optional<WikiArticle> lookup(const WikiSite &site, const std::string &word);

  private:
    std::optional<WikiArticle> fetch_page(const WikiSite &site, const std::string &page);
    void remember(const std::string &key, const std::optional<WikiArticle> &article);
    HttpFetch fetch_;
    std::mutex mutex_;
    std::map<std::string, std::optional<WikiArticle>> cache_; // site\nword -> article or miss
    std::vector<std::string> order_;
};
} // namespace dr
