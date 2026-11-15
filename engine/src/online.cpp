#include "dr/online.h"
#include "dr/text.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>
namespace dr
{
namespace
{
// Decodes one code point at `i` and advances past it; a malformed byte is taken as itself.
char32_t next_code_point(std::string_view s, size_t &i)
{
    const auto c = static_cast<unsigned char>(s[i]);
    int length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
    if (i + length > s.size())
        length = 1;
    char32_t cp = length == 1 ? c : length == 2 ? c & 0x1f : length == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < length; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    i += length;
    return cp;
}
bool is_han(char32_t c)
{
    return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x20000 && c <= 0x3134f);
}
bool is_latin_letter(char32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7);
}
bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

// The v2 error codes a user can act on (from DR Music's NiuTrans client).
std::string niutrans_error(const std::string &code, const std::string &message)
{
    if (code == "20001")
        return "小牛翻译鉴权失败，请检查 App ID 和 API Key";
    if (code == "13001")
        return "小牛翻译的字符额度不足，或该应用没有开通文本翻译";
    if (code == "10001")
        return "小牛翻译请求过于频繁，请稍后再试";
    if (code == "13007")
        return "小牛翻译不支持该语言";
    return "小牛翻译出错（" + code + "）" + (message.empty() ? "" : "：" + message);
}
} // namespace

std::string md5_hex(std::string_view data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("无法计算 MD5");
    unsigned char digest[16];
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status))
        throw std::runtime_error("无法计算 MD5");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char byte : digest)
    {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}

size_t code_points(std::string_view text)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++count)
        next_code_point(text, i);
    return count;
}

Direction translation_direction(std::string_view text)
{
    size_t han = 0, latin = 0;
    for (size_t i = 0; i < text.size();)
    {
        const auto c = next_code_point(text, i);
        if (is_han(c))
            ++han;
        else if (is_latin_letter(c))
            ++latin;
    }
    if (han > 0 && han * 3 >= latin)
        return {"zh", "en"};
    return {"en", "zh"};
}

NiuTrans::NiuTrans(HttpFetch fetch, std::function<long long()> clock) : fetch_(std::move(fetch)), clock_(std::move(clock))
{
    if (!clock_)
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
}
void NiuTrans::set_credentials(std::string app_id, std::string apikey)
{
    std::lock_guard lock(mutex_);
    app_id_ = std::move(app_id);
    apikey_ = std::move(apikey);
}
bool NiuTrans::configured() const
{
    std::lock_guard lock(mutex_);
    return !app_id_.empty() && !apikey_.empty();
}
std::string NiuTrans::app_id() const
{
    std::lock_guard lock(mutex_);
    return app_id_;
}
std::string NiuTrans::auth_str(const std::map<std::string, std::string> &params, const std::string &apikey)
{
    auto all = params;
    all["apikey"] = apikey;
    all.erase("authStr");
    std::string joined;
    for (const auto &[key, value] : all) // std::map iterates in ASCII key order
        if (!value.empty())
            joined += (joined.empty() ? "" : "&") + key + "=" + value;
    return md5_hex(joined);
}

Json NiuTrans::translate(const std::string &input, bool cached)
{
    const auto text = trim(input);
    if (text.empty())
        throw std::runtime_error("没有要翻译的文字");
    if (code_points(text) > max_length)
        throw std::runtime_error("文字过长，小牛翻译每次最多翻译 5000 个字符");
    const auto direction = translation_direction(text);
    const auto key = direction.from + ">" + direction.to + "\n" + text;
    auto result = [&](const std::string &translation) {
        return Json{{"text", text}, {"from", direction.from}, {"to", direction.to}, {"translation", translation}};
    };
    std::string app_id, apikey;
    std::chrono::steady_clock::duration wait{};
    {
        std::lock_guard lock(mutex_);
        if (cached)
            if (auto found = cache_.find(key); found != cache_.end())
                return result(found->second);
        app_id = app_id_;
        apikey = apikey_;
        if (app_id.empty() || apikey.empty())
            throw std::runtime_error("请先在“设置 → 在线”中填写小牛翻译的 App ID 和 API Key");
        // The free tier allows a few requests per second; keep well under it.
        const auto now = std::chrono::steady_clock::now();
        const auto next = std::max(now, last_request_ + std::chrono::milliseconds(250));
        wait = next - now;
        last_request_ = next;
    }
    if (wait > std::chrono::steady_clock::duration::zero())
        std::this_thread::sleep_for(wait);
    std::map<std::string, std::string> params = {{"from", direction.from},
                                                 {"to", direction.to},
                                                 {"srcText", text},
                                                 {"appId", app_id},
                                                 {"timestamp", std::to_string(clock_())}};
    params["authStr"] = auth_str(params, apikey);
    HttpRequest request{"POST", std::string("https://") + host + "/v2/text/translate",
                        {{"Content-Type", "application/json"}}, Json(params).dump()};
    request.limit = 1024 * 1024;
    const auto response = fetch_(request);
    const auto json = Json::parse(response.body, nullptr, false);
    if (!json.is_object())
        throw std::runtime_error("小牛翻译返回了无法识别的内容（HTTP " + std::to_string(response.status) + "）");
    auto field = [&](std::initializer_list<const char *> names) {
        for (auto name : names)
            if (json.contains(name) && !json[name].is_null())
            {
                const auto &value = json[name];
                return value.is_string() ? value.get<std::string>() : value.dump();
            }
        return std::string();
    };
    if (auto code = field({"errorCode", "error_code"}); !code.empty() && code != "0")
        throw std::runtime_error(niutrans_error(code, field({"errorMsg", "error_msg"})));
    if (!json.contains("tgtText") || !json["tgtText"].is_string())
        throw std::runtime_error("小牛翻译没有返回译文（HTTP " + std::to_string(response.status) + "）");
    auto translation = trim(json["tgtText"].get<std::string>());
    {
        std::lock_guard lock(mutex_);
        if (!cache_.count(key))
            cache_order_.push_back(key);
        cache_[key] = translation;
        while (cache_order_.size() > 200)
        {
            cache_.erase(cache_order_.front());
            cache_order_.erase(cache_order_.begin());
        }
    }
    return result(translation);
}

const std::vector<WikiSite> &wiki_sites()
{
    // Level-2 headings are language names; zh.wiktionary gives their ids in traditional
    // characters whatever the variant, so both writings are listed.
    static const std::vector<WikiSite> sites = {
        {"en", "English Wiktionary", "en.wiktionary.org", "", {"English", "Translingual", "Chinese"}},
        {"zh", "中文维基词典", "zh.wiktionary.org", "zh-cn", {"漢語", "汉语", "英語", "英语", "跨語言", "跨语言"}},
    };
    return sites;
}
const WikiSite *find_wiki_site(std::string_view id)
{
    for (const auto &site : wiki_sites())
        if (site.id == id)
            return &site;
    return nullptr;
}

std::string wiki_parse_url(const WikiSite &site, std::string_view page)
{
    // As GoldenDict-ng's MediaWikiArticleRequest::addQuery (src/dict/mediawiki.cc): action=parse
    // with redirects and the edition's variant. formatversion=2 gives the text as a plain string.
    std::string url = "https://" + site.host +
                      "/w/api.php?action=parse&format=json&formatversion=2&prop=text&redirects=1"
                      "&disableeditsection=1&disabletoc=1&disablelimitreport=1&page=" +
                      url_encode(page);
    if (!site.variant.empty())
        url += "&variant=" + url_encode(site.variant);
    return url;
}

std::optional<WikiArticle> parse_wiki_reply(const WikiSite &site, std::string_view body)
{
    const auto json = Json::parse(body, nullptr, false);
    if (!json.is_object())
        throw std::runtime_error("维基词典返回了无法识别的内容");
    // {"error": {"code": "missingtitle"}} for a word without a page.
    if (!json.contains("parse") || !json["parse"].is_object())
        return std::nullopt;
    const auto &parse = json["parse"];
    std::string html;
    if (parse.contains("text") && parse["text"].is_string())
        html = parse["text"].get<std::string>();
    else if (parse.contains("text") && parse["text"].is_object() && parse["text"].contains("*") &&
             parse["text"]["*"].is_string())
        html = parse["text"]["*"].get<std::string>(); // formatversion=1
    if (trim(html).empty())
        return std::nullopt;
    WikiArticle article;
    article.title = parse.value("title", std::string());
    article.html = rewrite_wiki_html(site, keep_wiki_languages(html, site.languages));
    return article;
}

std::string keep_wiki_languages(std::string_view html, const std::vector<std::string> &languages)
{
    static constexpr std::string_view marker = "<div class=\"mw-heading mw-heading2\">";
    std::vector<size_t> starts;
    for (auto at = html.find(marker); at != std::string_view::npos; at = html.find(marker, at + marker.size()))
        starts.push_back(at);
    if (starts.empty() || languages.empty())
        return std::string(html);
    std::string out(html.substr(0, starts[0]));
    bool kept = false, last_kept = false;
    for (size_t i = 0; i < starts.size(); ++i)
    {
        const auto end = i + 1 < starts.size() ? starts[i + 1] : html.size();
        const auto section = html.substr(starts[i], end - starts[i]);
        std::string id;
        const auto heading = section.find("<h2");
        const auto close = section.find('>', heading);
        if (heading != std::string_view::npos && close != std::string_view::npos)
        {
            const auto tag = section.substr(heading, close - heading);
            if (const auto at = tag.find(" id=\""); at != std::string_view::npos)
            {
                const auto quote = tag.find('"', at + 5);
                id = decode_entities(tag.substr(at + 5, quote == std::string_view::npos ? quote : quote - at - 5));
            }
        }
        last_kept = std::find(languages.begin(), languages.end(), id) != languages.end();
        if (last_kept)
        {
            out += section;
            kept = true;
        }
    }
    if (!kept)
        return std::string(html);
    // The last section ends with the </div> of the wrapper the preamble opened.
    if (!last_kept)
        out += "</div>";
    return out;
}

std::string rewrite_wiki_html(const WikiSite &site, std::string_view source)
{
    // The steps follow GoldenDict-ng's MediaWikiArticleRequest::requestFinished
    // (src/dict/mediawiki.cc): article links become lookups, other links and protocol-relative
    // media become absolute. That code applies a series of regex replacements; this is one pass
    // over the attributes instead, and it also drops scripts, which an article frame must not run.
    const std::string origin = "https://" + site.host;
    std::string html;
    {
        const auto lower = lower_ascii(source);
        size_t at = 0;
        for (auto open = lower.find("<script"); open != std::string::npos; open = lower.find("<script", at))
        {
            html.append(source.substr(at, open - at));
            const auto close = lower.find("</script>", open);
            at = close == std::string::npos ? source.size() : close + 9;
        }
        html.append(source.substr(std::min(at, source.size())));
    }
    auto absolute = [&](std::string_view value) -> std::string {
        if (starts_with(value, "//"))
            return "https:" + std::string(value);
        if (starts_with(value, "/"))
            return origin + std::string(value);
        return std::string(value);
    };
    auto link = [&](std::string_view value) -> std::string {
        if (!starts_with(value, "/wiki/"))
            return absolute(value);
        const auto rest = value.substr(6);
        const auto title = rest.substr(0, rest.find('#'));
        // Category:, Appendix:, File:, Wiktionary: … are wiki pages, not entries.
        if (title.empty() || url_decode(decode_entities(title)).find(':') != std::string::npos)
            return origin + std::string(value);
        std::string target = "entry://";
        for (char c : title)
            target += c == '_' ? std::string("%20") : std::string(1, c);
        return target;
    };
    auto srcset = [&](std::string_view value) {
        std::string out;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value.substr(i, 2) == "//" && (i == 0 || value[i - 1] == ' ' || value[i - 1] == ','))
                out += "https:";
            out += value[i];
        }
        return out;
    };
    auto style = [&](std::string_view value) {
        std::string out(value);
        for (std::string_view from : {"url(//", "url(&quot;//", "url('//", "url(\"//"})
        {
            const std::string to = std::string(from.substr(0, from.size() - 2)) + "https://";
            for (auto at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size()))
                out.replace(at, from.size(), to);
        }
        return out;
    };
    std::string out;
    out.reserve(html.size() + html.size() / 16);
    size_t i = 0;
    while (i < html.size())
    {
        const auto eq = html.find("=\"", i);
        if (eq == std::string::npos)
            break;
        const auto close = html.find('"', eq + 2);
        if (close == std::string::npos)
            break;
        size_t start = eq;
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(html[start - 1])) || html[start - 1] == '-'))
            --start;
        const auto name = lower_ascii(std::string_view(html).substr(start, eq - start));
        const bool attribute = start > 0 && std::isspace(static_cast<unsigned char>(html[start - 1]));
        const std::string_view value = std::string_view(html).substr(eq + 2, close - eq - 2);
        out.append(html, i, eq + 2 - i);
        if (attribute && name == "href")
            out += link(value);
        else if (attribute && name == "src")
            out += absolute(value);
        else if (attribute && name == "srcset")
            out += srcset(value);
        else if (attribute && name == "style")
            out += style(value);
        else
            out += value;
        out += '"';
        i = close + 1;
    }
    out.append(html, std::min(i, html.size()));
    return out;
}

std::string wiki_page(const WikiSite &site, const WikiArticle &article, const std::string &theme)
{
    std::string page_title = article.title;
    std::replace(page_title.begin(), page_title.end(), ' ', '_');
    const auto source = "https://" + site.host + "/wiki/" + url_encode(page_title);
    // Remote markup in a frame that shares the dictionaries' origin: nothing but the app's own
    // scripts may run, and media come only from Wikimedia's servers.
    return "<!DOCTYPE html><html data-dr-theme=\"" + html_escape(theme) + "\" lang=\"" + html_escape(site.id) +
           "\"><head><meta charset=\"utf-8\">"
           "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
           "script-src https://app.dr-dict.local; style-src https://app.dr-dict.local 'unsafe-inline'; "
           "img-src https://*.wikimedia.org data:; media-src https://upload.wikimedia.org\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
           "<link rel=\"stylesheet\" href=\"https://app.dr-dict.local/frame.css\">"
           "<link rel=\"stylesheet\" href=\"https://app.dr-dict.local/wiki.css\">"
           "<script src=\"https://app.dr-dict.local/frame.js\"></script>"
           "<script src=\"https://app.dr-dict.local/wiki.js\"></script></head><body><article class=\"dr-wiki\">" +
           article.html +
           "</article><p class=\"dr-wiki-source\">内容来自 " + html_escape(site.name) + " 的词条“<a href=\"" +
           html_escape(source) + "\">" + html_escape(article.title) +
           "</a>”，按 <a href=\"https://creativecommons.org/licenses/by-sa/4.0/deed.zh-hans\">CC BY-SA 4.0</a> "
           "协议授权。</p></body></html>";
}

Wiktionary::Wiktionary(HttpFetch fetch) : fetch_(std::move(fetch))
{
}

std::optional<WikiArticle> Wiktionary::fetch_page(const WikiSite &site, const std::string &page)
{
    HttpRequest request;
    request.url = wiki_parse_url(site, page);
    request.headers = {{"Accept", "application/json"}};
    const auto response = fetch_(request);
    if (response.status != 200)
        throw std::runtime_error(site.name + " 暂时无法访问（HTTP " + std::to_string(response.status) + "）");
    return parse_wiki_reply(site, response.body);
}

void Wiktionary::remember(const std::string &key, const std::optional<WikiArticle> &article)
{
    std::lock_guard lock(mutex_);
    if (!cache_.count(key))
        order_.push_back(key);
    cache_[key] = article;
    while (order_.size() > 64)
    {
        cache_.erase(order_.front());
        order_.erase(order_.begin());
    }
}

std::optional<WikiArticle> Wiktionary::lookup(const WikiSite &site, const std::string &input)
{
    const auto word = trim(input);
    // As GoldenDict-ng's MediaWikiDictionary::getArticle, long queries are not sent: no page has
    // such a title.
    if (word.empty() || code_points(word) > 80)
        return std::nullopt;
    const auto key = site.id + "\n" + word;
    {
        std::lock_guard lock(mutex_);
        if (auto found = cache_.find(key); found != cache_.end())
            return found->second;
    }
    // Titles are case-sensitive: "Paris" and "paris" are different pages. A miss tries the
    // lowercase form, or a lowercase word capitalised.
    std::vector<std::string> candidates = {word};
    if (auto lower = lower_text(word); lower != word)
        candidates.push_back(lower);
    else if (word[0] >= 'a' && word[0] <= 'z')
        candidates.push_back(static_cast<char>(word[0] - 'a' + 'A') + word.substr(1));
    std::optional<WikiArticle> article;
    for (const auto &candidate : candidates)
        if ((article = fetch_page(site, candidate)))
            break;
    remember(key, article);
    if (article && !article->title.empty())
        remember(site.id + "\n" + article->title, article);
    return article;
}
} // namespace dr
