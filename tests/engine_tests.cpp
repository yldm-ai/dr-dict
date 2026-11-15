#include "dr/compress.h"
#include "dr/library.h"
#include "dr/mdict.h"
#include "dr/online.h"
#include "dr/ripemd128.h"
#include "dr/stardict.h"
#include "dr/text.h"
#include <cstdio>
#include <iostream>
#include <miniz.h>
#include <windows.h>

using namespace dr;
static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n";                                \
        }                                                                                                              \
    } while (0)

namespace
{
std::string be(uint64_t v, int bytes)
{
    std::string s(bytes, '\0');
    for (int i = bytes - 1; i >= 0; --i, v >>= 8)
        s[i] = static_cast<char>(v & 0xff);
    return s;
}
std::string le32(uint32_t v)
{
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i)
        s[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    return s;
}
std::string utf16(std::string_view s)
{
    auto w = widen(s);
    return std::string(reinterpret_cast<const char *>(w.data()), w.size() * 2);
}
std::string zlib(std::string_view raw)
{
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string out(size, '\0');
    mz_compress(reinterpret_cast<unsigned char *>(out.data()), &size, reinterpret_cast<const unsigned char *>(raw.data()),
                static_cast<mz_ulong>(raw.size()));
    out.resize(size);
    return out;
}
std::string block(std::string_view raw)
{
    return std::string("\x02\0\0\0", 4) + be(adler32(raw), 4) + zlib(raw);
}
void encrypt_key_index(std::string &b)
{
    auto key = ripemd128(b.substr(4, 4) + std::string("\x95\x36\x00\x00", 4));
    uint8_t previous = 0x36;
    for (size_t i = 8; i < b.size(); ++i)
    {
        uint8_t plain = static_cast<uint8_t>(b[i]);
        uint8_t mixed = static_cast<uint8_t>(plain ^ previous ^ ((i - 8) & 0xff) ^ key[(i - 8) % 16]);
        uint8_t c = static_cast<uint8_t>((mixed >> 4) | (mixed << 4));
        b[i] = static_cast<char>(c);
        previous = c;
    }
}
// Writes a minimal MDict 2.0 file: one key block and one record block, zlib compressed.
void write_mdict(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries, bool mdd,
                 bool encrypted, const std::string &extra = "")
{
    std::string header = std::string(mdd ? "<Library_Data" : "<Dictionary") +
                         " GeneratedByEngineVersion=\"2.0\" RequiredEngineVersion=\"2.0\" Encrypted=\"" +
                         (encrypted ? "2" : "0") + "\" Encoding=\"" + (mdd ? "" : "UTF-8") + "\" " + extra + "/>\r\n";
    auto header16 = utf16(header);
    std::string out = be(header16.size(), 4) + header16 + le32(adler32(header16));
    std::string records, keys;
    for (auto &[key, value] : entries)
    {
        keys += be(records.size(), 8);
        keys += mdd ? utf16(key) + std::string(2, '\0') : key + std::string(1, '\0');
        records += value;
        if (!mdd)
            records.push_back('\0');
    }
    auto key_block = block(keys);
    auto text = [&](const std::string &s) {
        auto units = mdd ? widen(s).size() : s.size();
        return be(units, 2) + (mdd ? utf16(s) + std::string(2, '\0') : s + std::string(1, '\0'));
    };
    std::string info_raw = be(entries.size(), 8) + text(entries.front().first) + text(entries.back().first) +
                           be(key_block.size(), 8) + be(keys.size(), 8);
    auto info = block(info_raw);
    if (encrypted)
        encrypt_key_index(info);
    std::string kh = be(1, 8) + be(entries.size(), 8) + be(info_raw.size(), 8) + be(info.size(), 8) +
                     be(key_block.size(), 8);
    out += kh + be(adler32(kh), 4) + info + key_block;
    auto record_block = block(records);
    out += be(1, 8) + be(entries.size(), 8) + be(16, 8) + be(record_block.size(), 8);
    out += be(record_block.size(), 8) + be(records.size(), 8) + record_block;
    write_file(path, out);
}
std::string gzip(std::string_view raw)
{
    size_t size = 0;
    void *data = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
    std::string out = std::string("\x1f\x8b\x08\0\0\0\0\0\0\x03", 10) +
                      std::string(static_cast<const char *>(data), size) + le32(0) +
                      le32(static_cast<uint32_t>(raw.size()));
    mz_free(data);
    return out;
}
std::string hex(const std::array<uint8_t, 16> &d)
{
    std::string s;
    char buf[3];
    for (auto b : d)
    {
        snprintf(buf, sizeof buf, "%02x", b);
        s += buf;
    }
    return s;
}
} // namespace

int main()
{
    // RIPEMD-128 reference vectors from the algorithm's specification.
    CHECK(hex(ripemd128("")) == "cdf26213a150dc3ecb610f18f6b38b46");
    CHECK(hex(ripemd128("abc")) == "c14a12199c66e4ba84636b0f69144c77");
    CHECK(hex(ripemd128("message digest")) == "9e327b3d6e523062afc1132d7df9d1b8");

    // LZO1X: a literal run, an M2 back-reference and the end-of-stream marker.
    CHECK(lzo1x_decompress(std::string("\x16" "abcde" "\x11\0\0", 9), 5) == "abcde");
    CHECK(lzo1x_decompress(std::string("\x16" "abcde" "\x50\x00" "\x11\0\0", 11), 8) == "abcdeabc");

    CHECK(fold_key("Café") == fold_key("cafe"));
    CHECK(fold_key("Ice-Cream") == fold_key("icecream"));
    CHECK(fold_key("  Hello  ") == "hello");
    CHECK(fold_key("が") != fold_key("か"));
    CHECK(fold_key("+") == "+");
    CHECK(to_utf8(std::string("\xc4\xe3\xba\xc3", 4), "GBK") == "你好");
    CHECK(apply_mdict_styles("`1`bold`2`plain", {{1, {"<b>", "</b>"}}}) == "<b>bold</b>plain");
    CHECK(decode_entities("&lt;a&gt; &amp; &#x27;q&#39;") == "<a> & 'q'");
    auto forms = english_base_forms("studies");
    CHECK(std::find(forms.begin(), forms.end(), "study") != forms.end());
    forms = english_base_forms("running");
    CHECK(std::find(forms.begin(), forms.end(), "run") != forms.end());

    // Online sources, with a fake network.
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(code_points("a苹果") == 3);
    CHECK(translation_direction("今天天气很好。").from == "zh");
    CHECK(translation_direction("我喜欢 Python 编程").to == "en");
    CHECK(translation_direction("The weather is nice today.").from == "en");
    CHECK(translation_direction("I visited 北京 last year").to == "zh");
    // authStr: "key=value" pairs in key order, apikey included, empty values and authStr left out.
    CHECK(NiuTrans::auth_str({{"to", "zh"}, {"from", "en"}, {"empty", ""}, {"authStr", "x"}}, "k") ==
          md5_hex("apikey=k&from=en&to=zh"));
    {
        std::vector<HttpRequest> sent;
        NiuTrans niu(
            [&](const HttpRequest &request) {
                sent.push_back(request);
                return HttpResponse{200, R"({"from":"en","to":"zh","tgtText":" 今天天气很好。 "})"};
            },
            [] { return 1700000000000LL; });
        bool threw = false;
        try
        {
            niu.translate("Hello");
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        CHECK(threw && sent.empty()); // no credentials, no request
        niu.set_credentials("app", "key");
        auto result = niu.translate("  The weather is nice today.  ");
        CHECK(result["translation"] == "今天天气很好。");
        CHECK(result["from"] == "en" && result["to"] == "zh");
        CHECK(sent.size() == 1);
        if (!sent.empty())
        {
            CHECK(sent[0].method == "POST" && sent[0].url == "https://api.niutrans.com/v2/text/translate");
            auto body = Json::parse(sent[0].body);
            CHECK(body["srcText"] == "The weather is nice today." && body["appId"] == "app");
            CHECK(body["timestamp"] == "1700000000000");
            std::map<std::string, std::string> params;
            for (auto &[k, v] : body.items())
                params[k] = v.get<std::string>();
            CHECK(body["authStr"] == NiuTrans::auth_str(params, "key"));
        }
        niu.translate("The weather is nice today.");
        CHECK(sent.size() == 1); // remembered
        niu.translate("The weather is nice today.", false);
        CHECK(sent.size() == 2);
        NiuTrans denied([](const HttpRequest &) { return HttpResponse{200, R"({"errorCode":"20001","errorMsg":"auth"})"}; });
        denied.set_credentials("app", "bad");
        std::string message;
        try
        {
            denied.translate("你好");
        }
        catch (const std::runtime_error &e)
        {
            message = e.what();
        }
        CHECK(message.find("鉴权失败") != std::string::npos);
    }
    {
        const auto &en = *find_wiki_site("en");
        CHECK(find_wiki_site("xx") == nullptr);
        CHECK(wiki_parse_url(en, "ice cream").find("&page=ice%20cream") != std::string::npos);
        const std::string wrapper = "<div class=\"mw-content-ltr mw-parser-output\" lang=\"en\" dir=\"ltr\">";
        const std::string page =
            wrapper + "<div class=\"disambig-see-also\">See also</div>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"English\">English</h2></div><p>en</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"French\">French</h2></div><p>fr</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"Chinese\">Chinese</h2></div><p>zh</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"Dutch\">Dutch</h2></div><p>nl</p></div>";
        auto kept = keep_wiki_languages(page, en.languages);
        CHECK(kept.find("<p>en</p>") != std::string::npos && kept.find("<p>zh</p>") != std::string::npos);
        CHECK(kept.find("<p>fr</p>") == std::string::npos && kept.find("<p>nl</p>") == std::string::npos);
        CHECK(kept.find("See also") != std::string::npos);
        CHECK(kept.size() >= 6 && kept.substr(kept.size() - 6) == "</div>"); // the wrapper stays closed
        CHECK(keep_wiki_languages(page, {"Latin"}) == page);                  // nothing matched: whole page

        auto html = rewrite_wiki_html(
            en, "<a href=\"/wiki/ice_cream#English\" title=\"x\">i</a><a href=\"/wiki/Appendix:Glossary#dated\">g</a>"
                "<a href=\"/w/index.php?title=x&amp;action=edit\">e</a><a href=\"#Noun\">n</a>"
                "<a href=\"https://en.wikipedia.org/wiki/hello\">w</a><img src=\"//upload.wikimedia.org/a.png\" "
                "srcset=\"//upload.wikimedia.org/b.png 1.5x, //upload.wikimedia.org/c.png 2x\">"
                "<span style=\"background:url(//upload.wikimedia.org/d.svg)\"></span>"
                "<script>alert(1)</script><SCRIPT src=\"x\"></SCRIPT><p>x=\"y\" stays</p>");
        CHECK(html.find("href=\"entry://ice%20cream\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wiktionary.org/wiki/Appendix:Glossary#dated\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wiktionary.org/w/index.php?title=x&amp;action=edit\"") != std::string::npos);
        CHECK(html.find("href=\"#Noun\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wikipedia.org/wiki/hello\"") != std::string::npos);
        CHECK(html.find("src=\"https://upload.wikimedia.org/a.png\"") != std::string::npos);
        CHECK(html.find("srcset=\"https://upload.wikimedia.org/b.png 1.5x, https://upload.wikimedia.org/c.png 2x\"") !=
              std::string::npos);
        CHECK(html.find("url(https://upload.wikimedia.org/d.svg)") != std::string::npos);
        CHECK(html.find("alert") == std::string::npos && lower_ascii(html).find("<script") == std::string::npos);
        CHECK(html.find("<p>x=\"y\" stays</p>") != std::string::npos);

        CHECK(!parse_wiki_reply(en, R"({"error":{"code":"missingtitle"}})"));
        auto article = parse_wiki_reply(en, Json{{"parse", {{"title", "hello"}, {"pageid", 1}, {"text", page}}}}.dump());
        CHECK(article && article->title == "hello" && article->html.find("<p>fr</p>") == std::string::npos);
        if (article)
        {
            auto frame = wiki_page(en, *article, "dark");
            CHECK(frame.find("data-dr-theme=\"dark\"") != std::string::npos);
            CHECK(frame.find("Content-Security-Policy") != std::string::npos);
            CHECK(frame.find("https://en.wiktionary.org/wiki/hello") != std::string::npos);
        }

        // "Hello" has no page of its own: the lowercase title is tried, and both are remembered.
        std::vector<std::string> asked;
        Wiktionary wiki([&](const HttpRequest &request) {
            asked.push_back(request.url);
            if (request.url.find("&page=hello") != std::string::npos)
                return HttpResponse{200, Json{{"parse", {{"title", "hello"}, {"text", "<p>hi</p>"}}}}.dump()};
            return HttpResponse{200, R"({"error":{"code":"missingtitle"}})"};
        });
        auto found = wiki.lookup(en, "Hello");
        CHECK(found && found->title == "hello" && asked.size() == 2);
        CHECK(wiki.lookup(en, "Hello") && wiki.lookup(en, "hello") && asked.size() == 2);
        CHECK(!wiki.lookup(en, "zzqq") && asked.size() == 4); // zzqq, then Zzqq
        CHECK(!wiki.lookup(en, "zzqq") && asked.size() == 4); // misses are remembered too
        CHECK(!wiki.lookup(en, std::string(81, 'a')) && asked.size() == 4);
    }

    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    auto root = fs::path(temp) / ("dr-dict-tests-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    auto dicts = root / "dicts";
    fs::create_directories(dicts / "sd" / "res");

    write_mdict(dicts / "demo.mdx",
                {{"apple", "<link rel=\"stylesheet\" href=\"demo.css\"><b>apple</b> 苹果 `1`fruit`0`"},
                 {"Banana", "<i>banana</i> 香蕉"},
                 {"pomme", "@@@LINK=apple"}},
                false, true, "Title=\"Demo &amp; Test\" Description=\"&lt;p&gt;About&lt;/p&gt;\" StyleSheet=\"1\n<em>\n</em>\n\"");
    // OALDPE names example audio like "_take#_gbs_1.mp3" (linked as sound://_take#_gbs_1.mp3).
    write_mdict(dicts / "demo.mdd",
                {{"\\demo.css", "b{color:red}"}, {"\\img\\A.png", "PNG"}, {"\\_take#_gbs_1.mp3", "MP3"}}, true, false);
    {
        MdictDictionary d(dicts / "demo.mdx");
        CHECK(d.title() == "Demo & Test");
        CHECK(d.description() == "<p>About</p>");
        std::vector<std::tuple<std::string, uint64_t, uint64_t>> words;
        IndexSink sink;
        sink.word = [&](std::string_view w, uint64_t a, uint64_t b) { words.emplace_back(std::string(w), a, b); };
        sink.resource = [&](std::string_view p, int, uint64_t, uint64_t) { words.emplace_back("res:" + std::string(p), 0, 0); };
        d.index(sink);
        CHECK(words.size() == 6);
        CHECK(std::get<0>(words[0]) == "apple");
        CHECK(d.article(std::get<1>(words[0]), std::get<2>(words[0])).find("<em>fruit</em>") != std::string::npos);
        CHECK(d.article(std::get<1>(words[2]), std::get<2>(words[2])) == "@@@LINK=apple");
        CHECK(std::get<0>(words[4]) == "res:img/A.png");
    }

    // StarDict with a .syn file and a gzip-compressed .dict.dz without a random-access field.
    std::string dict_data = std::string("hello world\nsecond line") + "goodbye";
    std::string idx = std::string("goodbye") + '\0' + be(23, 4) + be(7, 4) + "hello" + '\0' + be(0, 4) + be(23, 4);
    write_file(dicts / "sd" / "test.ifo", "StarDict's dict ifo file\nversion=2.4.2\nwordcount=2\nidxfilesize=" +
                                               std::to_string(idx.size()) + "\nbookname=Star Test\nsametypesequence=m\n");
    write_file(dicts / "sd" / "test.idx", idx);
    write_file(dicts / "sd" / "test.dict.dz", gzip(dict_data));
    write_file(dicts / "sd" / "test.syn", std::string("hi") + '\0' + be(1, 4));
    write_file(dicts / "sd" / "res" / "pic.png", "IMG");
    CHECK(stardict_to_html(std::string("thalo\0mmeaning", 14), "") ==
          "<div class=\"sd-t\">[halo]</div><div class=\"sd-m\">meaning</div>");

    {
        Library library(root / "data");
        auto result = library.add({dicts}, {});
        CHECK(result["added"] == 2);
        CHECK(result["errors"].empty());
        auto again = library.add({dicts / "demo.mdx"}, {});
        CHECK(again["skipped"] == 1);
        auto list = library.list();
        CHECK(list.size() == 2);
        std::string mdx_id, sd_id;
        for (auto &d : list)
            (d["format"] == "mdict" ? mdx_id : sd_id) = d["id"];
        CHECK(list[0]["status"] == "ready");

        auto suggestions = library.suggest("ba", 10);
        CHECK(suggestions.size() == 1 && suggestions[0] == "Banana");
        auto hit = library.lookup("APPLE");
        CHECK(hit["results"].size() == 1);
        auto plural = library.lookup("apples");
        CHECK(plural["word"] == "apples" || plural["word"] == "apple");
        CHECK(plural["results"].size() == 1);
        auto syn = library.lookup("hi");
        CHECK(syn["results"].size() == 1);
        // A search can be limited to some dictionaries.
        CHECK(library.lookup("apple", {sd_id})["results"].empty());
        CHECK(library.lookup("apple", {mdx_id})["results"].size() == 1);
        CHECK(library.suggest("ba", 10, {sd_id}).empty() && library.suggest("ba", 10, {mdx_id}).size() == 1);

        auto page = library.article_page(mdx_id, "pomme", "light");
        CHECK(page.find("苹果") != std::string::npos);
        CHECK(page.find("frame.js") != std::string::npos);
        auto star = library.article_page(sd_id, "hello", "dark");
        CHECK(star.find("hello world<br>second line") != std::string::npos);
        CHECK(library.article_page(sd_id, "goodbye", "light").find("goodbye") != std::string::npos);

        auto css = library.resource(mdx_id, "demo.css");
        CHECK(css && css->data == "b{color:red}" && css->mime == "text/css");
        auto img = library.resource(mdx_id, "IMG/a.png?x=1");
        CHECK(img && img->data == "PNG");
        CHECK(!library.resource(mdx_id, "../secret.txt"));
        // frame.js percent-encodes sound:// names, so a '#' inside the name arrives as %23; a raw '#'
        // is a URL fragment and must not reach the name.
        auto voice = library.resource(mdx_id, "_take%23_gbs_1.mp3");
        CHECK(voice && voice->data == "MP3" && voice->mime == "audio/mpeg");
        CHECK(!library.resource(mdx_id, "_take#_gbs_1.mp3"));
        auto pic = library.resource(sd_id, "pic.png");
        CHECK(pic && pic->data == "IMG");

        library.update(mdx_id, {{"enabled", false}});
        CHECK(library.lookup("apple")["results"].empty());
        library.update(mdx_id, {{"enabled", true}});
        auto order = library.reorder(Json::array({sd_id, mdx_id}));
        CHECK(order[0]["id"] == sd_id);
        CHECK(library.reindex(mdx_id, {}).size() == 2);
        CHECK(library.lookup("banana")["results"].size() == 1);
        library.remove(mdx_id);
        auto after = library.list();
        CHECK(after.size() == 2); // the removed dictionary is still listed …
        for (auto &d : after)
            if (d["id"] == mdx_id)
                CHECK(d["status"] == "removed" && d["enabled"] == false && d["fileMissing"] == false);
        CHECK(library.lookup("banana")["results"].empty()); // … but not looked up
        CHECK(library.suggest("ban", 10).empty());
        bool rejected = false;
        try
        {
            library.update(mdx_id, {{"enabled", true}});
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        CHECK(rejected);
        rejected = false;
        try
        {
            library.forget(mdx_id); // its file still exists
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        CHECK(rejected);
        library.restore(mdx_id, {});
        CHECK(library.lookup("banana")["results"].size() == 1);
        library.remove(mdx_id);

        library.remember("apple");
        library.remember("pear");
        auto history = library.remember("apple");
        CHECK(history.size() == 2 && history[0]["word"] == "apple");
        history = library.forget_word("apple");
        CHECK(history.size() == 1 && history[0]["word"] == "pear" && library.history().size() == 1);
        CHECK(library.favorites().empty());
        library.toggle_favorite("apple");
        auto stars = library.toggle_favorite("pear");
        CHECK(stars.size() == 2 && stars[0]["word"] == "pear");
        stars = library.toggle_favorite("apple"); // starring again unstars
        CHECK(stars.size() == 1 && library.favorites()[0]["word"] == "pear");
        CHECK(library.save_settings({{"volume", 3}})["volume"] == 1.0);
        CHECK(library.save_settings({{"volume", 0.5}})["volume"] == 0.5);
        auto scoped = library.save_settings({{"scope", Json::array({mdx_id, 3})}});
        CHECK(scoped["scope"] == Json::array({mdx_id}));
        auto settings = library.save_settings({{"theme", "dark"}, {"shell", "bogus"}});
        CHECK(settings["theme"] == "dark" && settings["shell"] == "mica");

        // Appearance: built-in schemes, custom colours, background image.
        CHECK(library.save_settings({{"theme", "night"}})["theme"] == "night");
        CHECK(library.save_settings({{"theme", "neon"}})["theme"] == "night");
        CHECK(theme_mode("night") == "dark" && theme_mode("paper") == "light" && theme_mode("system") == "system");
        auto colors = library.save_settings({{"backgroundColor", "#1A2b3c"}, {"accentColor", "red"}});
        CHECK(colors["backgroundColor"] == "#1A2b3c" && colors["accentColor"] == "");
        CHECK(library.save_settings({{"backgroundColor", ""}})["backgroundColor"] == "");
        auto look = library.save_settings({{"backgroundOpacity", 2}, {"backgroundBlur", 99}, {"backgroundImage", "../x"}});
        CHECK(look["backgroundOpacity"] == 1.0 && look["backgroundBlur"] == 40 && look["backgroundImage"] == "");
        // Ctrl+C+C popup: on by default; its size is clamped.
        CHECK(Library::default_settings()["scanPopup"] == true);
        auto popup = library.save_settings({{"scanPopup", false}, {"popupWidth", 10}, {"popupHeight", 9999}});
        CHECK(popup["scanPopup"] == false && popup["popupWidth"] == 280 && popup["popupHeight"] == 1400);
        library.save_settings({{"backgroundBlur", 12}});
        CHECK(library.settings()["backgroundBlur"] == 12); // read back from settings.json
        write_file(dicts / "wall.PNG", "PNGDATA");
        write_file(dicts / "notes.txt", "x");
        bool bad_image = false;
        try
        {
            library.set_background_image(dicts / "notes.txt");
        }
        catch (const std::exception &)
        {
            bad_image = true;
        }
        CHECK(bad_image);
        auto first = library.set_background_image(dicts / "wall.PNG")["backgroundImage"].get<std::string>();
        CHECK(first.ends_with(".png") && fs::exists(root / "data" / "backgrounds" / first));
        auto image = library.background_image(first);
        CHECK(image && image->data == "PNGDATA" && image->mime == "image/png");
        CHECK(!library.background_image("../settings.json") && !library.background_image("other.png"));
        auto second = library.set_background_image(dicts / "wall.PNG")["backgroundImage"].get<std::string>();
        CHECK(second != first && !fs::exists(root / "data" / "backgrounds" / first)); // old copy removed
        CHECK(library.clear_background_image()["backgroundImage"] == "");
        CHECK(!fs::exists(root / "data" / "backgrounds" / second) && !library.background_image(second));
        CHECK(fs::exists(dicts / "wall.PNG")); // the original is untouched
    }

    // Remembered folders: a rescan picks up new dictionaries; removed ones stay in the removed group.
    {
        auto shelf = dicts / "shelf";
        write_mdict(shelf / "one.mdx", {{"alpha", "<p>a</p>"}}, false, false);
        Library lib(root / "shelf-data");
        CHECK(lib.add({shelf}, {})["added"] == 1);
        CHECK(lib.folders().size() == 1 && lib.folders()[0]["exists"] == true);
        write_mdict(shelf / "deeper" / "two.mdx", {{"beta", "<p>b</p>"}}, false, false);
        auto rescanned = lib.rescan({});
        CHECK(rescanned["added"] == 1 && rescanned["folders"] == 1 && rescanned["errors"].empty());
        CHECK(lib.lookup("beta")["results"].size() == 1);
        CHECK(lib.rescan({})["added"] == 0); // nothing new

        std::string two_id;
        for (auto &d : lib.list())
            if (d["path"].get<std::string>().find("two.mdx") != std::string::npos)
                two_id = d["id"];
        auto removed_count = [&] {
            int n = 0;
            for (auto &d : lib.list())
                n += d["status"] == "removed";
            return n;
        };
        lib.remove(two_id);
        CHECK(lib.rescan({})["added"] == 0 && removed_count() == 1); // removed stays removed
        CHECK(lib.add({shelf}, {})["added"] == 0 && removed_count() == 1); // even when its folder is added again
        CHECK(lib.lookup("beta")["results"].empty());
        CHECK(lib.add({shelf / "deeper" / "two.mdx"}, {})["added"] == 1); // picking its file restores it
        CHECK(removed_count() == 0 && lib.lookup("beta")["results"].size() == 1);

        // Once the file of a removed dictionary is gone, its entry can be cleared.
        lib.remove(two_id);
        fs::remove_all(shelf / "deeper");
        CHECK(lib.forget(two_id).size() == 1 && removed_count() == 0);
        CHECK(lib.list().size() == 1);

        fs::create_directories(shelf / "deeper");
        lib.add({shelf / "deeper"}, {}); // inside a remembered folder: nothing new to remember
        CHECK(lib.folders().size() == 1);
        lib.add({dicts}, {}); // a parent replaces the folders inside it
        CHECK(lib.folders().size() == 1 && lib.folders()[0]["path"].get<std::string>().find("shelf") == std::string::npos);
        CHECK(lib.remove_folder(lib.folders()[0]["path"]).empty());
        CHECK(lib.rescan({})["folders"] == 0);
    }

    // Simplified/traditional lookups through OpenCC, whose data the build copies beside this binary.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        auto opencc = fs::path(exe).parent_path() / "opencc";
        auto has = [](const std::vector<std::string> &list, const std::string &value) {
            return std::find(list.begin(), list.end(), value) != list.end();
        };
        ChineseVariants cc(opencc);
        CHECK(cc.available());
        CHECK(has(cc.expand("软件", "t"), "軟件") && !has(cc.expand("软件", "t"), "軟體"));
        CHECK(has(cc.expand("软件", "twp"), "軟體"));
        CHECK(has(cc.expand("软件", "all"), "軟件") && has(cc.expand("软件", "all"), "軟體"));
        CHECK(has(cc.expand("軟體", "all"), "软件")); // traditional input reaches simplified too
        CHECK(has(cc.expand("里面", "t"), "裏面") && has(cc.expand("里面", "tw"), "裡面"));
        CHECK(cc.expand("apple", "all").empty() && cc.expand("软件", "bogus").empty());
        CHECK(has_han("A 字") && !has_han("abc") && !has_han("かな"));
        // Article text switches script; markup, resource names, scripts and styles do not.
        auto html = cc.convert_html(
            "<p class=\"長\" title='a>長'><img src=\"長.png\">長度 a<b <!--長--><script>var s='長';</script>"
            "<STYLE>.長{}</STYLE><b>长</b></p>",
            "s", "t");
        CHECK(html == "<p class=\"長\" title='a>長'><img src=\"長.png\">长度 a<b <!--長--><script>var s='長';</script>"
                      "<STYLE>.長{}</STYLE><b>长</b></p>");
        CHECK(cc.convert_html("<i>软件</i>", "t", "twp") == "<i>軟體</i>");
        CHECK(cc.convert_html("<i>软件</i>", "t", "t") == "<i>軟件</i>");
        CHECK(cc.convert_html("<i>軟</i>", "x", "t") == "<i>軟</i>");

        // A dictionary written only in Taiwan forms.
        write_mdict(dicts / "zh" / "zh.mdx", {{"軟體", "<p>software</p>"}, {"裏面", "<p>inside</p>"}}, false, false);
        Library zh(root / "zh-data");
        zh.set_opencc_dir(opencc);
        CHECK(zh.chinese_available());
        CHECK(zh.add({dicts / "zh" / "zh.mdx"}, {})["added"] == 1);
        CHECK(zh.lookup("软件")["results"].empty()); // off by default
        zh.save_settings({{"chineseConversion", true}, {"chineseVariant", "tw"}});
        CHECK(zh.lookup("软件")["results"].empty()); // Taiwan characters alone give 軟件
        CHECK(zh.save_settings({{"chineseVariant", "bogus"}})["chineseVariant"] == "tw");
        zh.save_settings({{"chineseVariant", "twp"}});
        auto hit = zh.lookup("软件");
        CHECK(hit["results"].size() == 1 && hit["variants"] == Json::array({"軟體"}));
        if (!hit["results"].empty())
        {
            auto zh_id = hit["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(zh_id, "软件", "light").find("software") != std::string::npos);
        }
        write_mdict(dicts / "zh2" / "zh2.mdx", {{"長", "<p>長度</p>"}}, false, false);
        CHECK(zh.add({dicts / "zh2" / "zh2.mdx"}, {})["added"] == 1);
        auto long_hit = zh.lookup("長");
        CHECK(!long_hit["results"].empty());
        if (!long_hit["results"].empty())
        {
            auto id = long_hit["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(id, "長", "light", "s").find("<p>长度</p>") != std::string::npos);
            CHECK(zh.article_page(id, "長", "light").find("<p>長度</p>") != std::string::npos);
        }
        // 繁: the standard chosen in the header, else the "traditionalVariant" setting.
        write_mdict(dicts / "zh3" / "zh3.mdx", {{"软", "<p>软件</p>"}}, false, false);
        CHECK(zh.add({dicts / "zh3" / "zh3.mdx"}, {})["added"] == 1);
        CHECK(Library::default_settings()["traditionalVariant"] == "t");
        CHECK(zh.save_settings({{"traditionalVariant", "all"}})["traditionalVariant"] == "t");
        auto soft = zh.lookup("软");
        CHECK(!soft["results"].empty());
        if (!soft["results"].empty())
        {
            auto id = soft["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(id, "软", "light", "t").find("<p>軟件</p>") != std::string::npos);
            CHECK(zh.article_page(id, "软", "light", "t", "twp").find("<p>軟體</p>") != std::string::npos);
            zh.save_settings({{"traditionalVariant", "twp"}});
            CHECK(zh.article_page(id, "软", "light", "t").find("<p>軟體</p>") != std::string::npos);
            CHECK(zh.article_page(id, "软", "light", "t", "t").find("<p>軟件</p>") != std::string::npos);
        }
        // Online settings: known Wiktionary editions only, once each, in a fixed order.
        CHECK(zh.settings()["wiktionary"] == Json::array({"en"}));
        CHECK(zh.save_settings({{"wiktionary", {"zh", "xx", "en", "zh"}}})["wiktionary"] == Json::array({"en", "zh"}));
        CHECK(zh.save_settings({{"wiktionary", Json::array()}, {"translateSentences", false}})["wiktionary"].empty());
        CHECK(zh.settings()["translateSentences"] == false);
        zh.save_settings({{"chineseVariant", "t"}});
        CHECK(zh.lookup("里面")["results"].size() == 1);
        zh.save_settings({{"chineseVariant", "all"}});
        auto suggestions = zh.suggest("软", 10);
        CHECK(std::find(suggestions.begin(), suggestions.end(), "軟體") != suggestions.end());
    }
    fs::remove_all(root);
    if (failures)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "engine tests passed\n";
    return 0;
}
