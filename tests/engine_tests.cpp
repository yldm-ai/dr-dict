#include "dr/compress.h"
#include "dr/library.h"
#include "dr/mdict.h"
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
        auto settings = library.save_settings({{"theme", "dark"}, {"shell", "bogus"}});
        CHECK(settings["theme"] == "dark" && settings["shell"] == "mica");
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
