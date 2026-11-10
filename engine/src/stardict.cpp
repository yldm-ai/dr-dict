#include "dr/stardict.h"
#include "dr/mdict.h"
#include "dr/text.h"
#include <regex>
#include <stdexcept>
namespace dr
{
namespace
{
uint64_t big_endian(std::string_view s, size_t at, int bytes)
{
    if (at + bytes > s.size())
        throw std::runtime_error("StarDict 索引已损坏");
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v = (v << 8) | static_cast<unsigned char>(s[at + i]);
    return v;
}
fs::path sibling(const fs::path &ifo, const char *suffix)
{
    auto p = ifo;
    p.replace_extension();
    return p.concat(suffix);
}
std::string text_block(std::string_view s, const char *css)
{
    std::string html = "<div class=\"" + std::string(css) + "\">";
    for (char c : html_escape(s))
        if (c == '\n')
            html += "<br>";
        else
            html.push_back(c);
    return html + "</div>";
}
std::string xdxf_to_html(std::string_view s)
{
    std::string h(s);
    static const std::regex kref(R"(<kref[^>]*>([\s\S]*?)</kref>)");
    h = std::regex_replace(h, kref, "<a class=\"xdxf-kref\" href=\"entry://$1\">$1</a>");
    static const std::regex color(R"(<c\s+c=["']?([#\w]+)["']?\s*>)");
    h = std::regex_replace(h, color, "<span style=\"color:$1\">");
    static const std::regex tr_open(R"(<tr>)"), tr_close(R"(</tr>)");
    h = std::regex_replace(h, tr_open, "<span class=\"xdxf-tr\">[");
    h = std::regex_replace(h, tr_close, "]</span>");
    static const std::regex open(R"(<(k|c|ex|co|dtrn|abr|pos|gr|nu|def|deftext|rref|sr|syn|ant)(\s[^>]*)?>)");
    h = std::regex_replace(h, open, "<span class=\"xdxf-$1\">");
    static const std::regex close(R"(</(k|c|ex|co|dtrn|abr|pos|gr|nu|def|deftext|rref|sr|syn|ant)>)");
    h = std::regex_replace(h, close, "</span>");
    static const std::regex iref(R"(<iref\s+href=)");
    h = std::regex_replace(h, iref, "<a href=");
    static const std::regex iref_close(R"(</iref>)");
    h = std::regex_replace(h, iref_close, "</a>");
    std::string out;
    for (char c : h)
        if (c == '\n')
            out += "<br>";
        else
            out.push_back(c);
    return "<div class=\"sd-x\">" + out + "</div>";
}
std::string pango_to_html(std::string_view s)
{
    std::string h(s);
    static const std::regex fg(R"(\bforeground=["']([^"']*)["'])"), bg(R"(\bbackground=["']([^"']*)["'])");
    h = std::regex_replace(h, fg, "style=\"color:$1\"");
    h = std::regex_replace(h, bg, "style=\"background:$1\"");
    std::string out;
    for (char c : h)
        if (c == '\n')
            out += "<br>";
        else
            out.push_back(c);
    return "<div class=\"sd-g\">" + out + "</div>";
}
std::string resource_list(std::string_view s)
{
    std::string html = "<div class=\"sd-r\">";
    size_t start = 0;
    while (start < s.size())
    {
        auto end = s.find('\n', start);
        auto line = trim(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        start = end == std::string_view::npos ? s.size() : end + 1;
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        auto kind = line.substr(0, colon);
        auto path = html_escape(line.substr(colon + 1));
        if (kind == "img")
            html += "<img src=\"" + path + "\" alt=\"\">";
        else if (kind == "snd")
            html += "<a class=\"sd-sound\" href=\"sound://" + path + "\">&#x1f50a;</a>";
        else if (kind == "vdo")
            html += "<video controls src=\"" + path + "\"></video>";
        else
            html += "<a href=\"" + path + "\">" + path + "</a>";
    }
    return html + "</div>";
}
std::string field_html(char type, std::string_view data)
{
    switch (type)
    {
    case 'h': return "<div class=\"sd-h\">" + std::string(data) + "</div>";
    case 'g': return pango_to_html(data);
    case 'x': return xdxf_to_html(data);
    case 't': return "<div class=\"sd-t\">[" + html_escape(data) + "]</div>";
    case 'l': return text_block(to_utf8(data, "cp0"), "sd-m");
    case 'r': return resource_list(data);
    case 'W': return "<div class=\"sd-binary\">（内嵌音频）</div>";
    case 'P': return "<div class=\"sd-binary\">（内嵌图片）</div>";
    default:
        if (type >= 'a' && type <= 'z')
            return text_block(data, "sd-m");
        return {};
    }
}
} // namespace

std::string stardict_to_html(std::string_view record, std::string_view seq)
{
    std::string html;
    size_t p = 0;
    auto take_text = [&](bool last) {
        if (last)
        {
            auto v = record.substr(p);
            p = record.size();
            while (!v.empty() && v.back() == '\0')
                v.remove_suffix(1);
            return v;
        }
        auto end = record.find('\0', p);
        if (end == std::string_view::npos)
            end = record.size();
        auto v = record.substr(p, end - p);
        p = std::min(record.size(), end + 1);
        return v;
    };
    auto take_binary = [&](bool last) {
        if (last)
        {
            auto v = record.substr(p);
            p = record.size();
            return v;
        }
        auto size = static_cast<size_t>(big_endian(record, p, 4));
        p += 4;
        auto v = record.substr(p, std::min(size, record.size() - p));
        p += v.size();
        return v;
    };
    if (!seq.empty())
    {
        for (size_t i = 0; i < seq.size() && p <= record.size(); ++i)
        {
            const bool last = i + 1 == seq.size();
            const char type = seq[i];
            auto data = (type >= 'a' && type <= 'z') ? take_text(last) : take_binary(last);
            html += field_html(type, data);
        }
        return html;
    }
    while (p < record.size())
    {
        const char type = record[p++];
        auto data = (type >= 'a' && type <= 'z') ? take_text(false) : take_binary(false);
        html += field_html(type, data);
    }
    return html;
}

StardictDictionary::StardictDictionary(const fs::path &ifo) : ifo_(ifo)
{
    auto text = read_file(ifo);
    if (!text.starts_with("StarDict's dict ifo file"))
        throw std::runtime_error("不是有效的 StarDict .ifo 文件");
    size_t start = 0;
    while (start < text.size())
    {
        auto end = text.find('\n', start);
        auto line = trim(std::string_view(text).substr(start, end == std::string::npos ? std::string::npos : end - start));
        start = end == std::string::npos ? text.size() : end + 1;
        auto eq = line.find('=');
        if (eq != std::string::npos)
            info_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    std::error_code ec;
    idx_ = sibling(ifo, ".idx");
    if (!fs::exists(idx_, ec))
        idx_ = sibling(ifo, ".idx.gz");
    if (!fs::exists(idx_, ec))
        throw std::runtime_error("缺少 StarDict 索引文件 (.idx)");
    dict_ = sibling(ifo, ".dict");
    if (fs::exists(dict_, ec))
        plain_ = std::make_unique<BinaryFile>(dict_);
    else
    {
        dict_ = sibling(ifo, ".dict.dz");
        if (!fs::exists(dict_, ec))
            throw std::runtime_error("缺少 StarDict 数据文件 (.dict/.dict.dz)");
        zipped_ = std::make_unique<DictZip>(dict_);
    }
    auto syn = sibling(ifo, ".syn");
    if (fs::exists(syn, ec))
        syn_ = syn;
}

std::string StardictDictionary::title() const
{
    auto it = info_.find("bookname");
    return it != info_.end() && !it->second.empty() ? it->second : utf8(ifo_.stem());
}

std::string StardictDictionary::description() const
{
    std::string html;
    for (auto key : {"author", "email", "website", "date"})
        if (auto it = info_.find(key); it != info_.end() && !it->second.empty())
            html += "<p><b>" + std::string(key) + "</b>: " + html_escape(it->second) + "</p>";
    if (auto it = info_.find("description"); it != info_.end())
        html += "<div>" + it->second + "</div>";
    return html;
}

void StardictDictionary::index(const IndexSink &sink)
{
    auto data = read_file(idx_);
    if (lower_ascii(idx_.extension().string()) == ".gz")
        data = gunzip(data);
    const int offset_bytes = info_.count("idxoffsetbits") && info_.at("idxoffsetbits") == "64" ? 8 : 4;
    std::vector<std::pair<uint64_t, uint32_t>> entries;
    size_t p = 0;
    while (p < data.size())
    {
        auto end = data.find('\0', p);
        if (end == std::string::npos)
            break;
        auto word = std::string_view(data).substr(p, end - p);
        p = end + 1;
        auto offset = big_endian(data, p, offset_bytes);
        p += offset_bytes;
        auto size = static_cast<uint32_t>(big_endian(data, p, 4));
        p += 4;
        entries.emplace_back(offset, size);
        if (sink.word && !word.empty())
            sink.word(word, offset, size);
        if (sink.progress && entries.size() % 4096 == 0)
            sink.progress(0.9 * double(p) / double(data.size()));
    }
    if (!syn_.empty() && sink.word)
    {
        auto syn = read_file(syn_);
        size_t q = 0;
        while (q < syn.size())
        {
            auto end = syn.find('\0', q);
            if (end == std::string::npos || end + 5 > syn.size())
                break;
            auto word = std::string_view(syn).substr(q, end - q);
            auto index = static_cast<size_t>(big_endian(syn, end + 1, 4));
            q = end + 5;
            if (index < entries.size() && !word.empty())
                sink.word(word, entries[index].first, entries[index].second);
        }
    }
    if (sink.progress)
        sink.progress(1.0);
}

std::string StardictDictionary::article(uint64_t a, uint64_t b)
{
    auto record = plain_ ? plain_->read(a, static_cast<size_t>(b)) : zipped_->read(a, static_cast<size_t>(b));
    auto seq = info_.count("sametypesequence") ? info_.at("sametypesequence") : std::string();
    return stardict_to_html(record, seq);
}

std::optional<fs::path> StardictDictionary::icon() const
{
    std::error_code ec;
    for (auto ext : {".png", ".jpg", ".ico", ".bmp", ".gif"})
    {
        auto candidate = sibling(ifo_, ext);
        if (fs::is_regular_file(candidate, ec))
            return candidate;
    }
    return std::nullopt;
}

std::vector<fs::path> StardictDictionary::files() const
{
    std::vector<fs::path> result{ifo_, idx_, dict_};
    if (!syn_.empty())
        result.push_back(syn_);
    return result;
}

bool is_dictionary_file(const fs::path &path)
{
    auto ext = lower_ascii(path.extension().string());
    return ext == ".mdx" || ext == ".ifo";
}

std::unique_ptr<Dictionary> open_dictionary(const fs::path &main_file)
{
    auto ext = lower_ascii(main_file.extension().string());
    if (ext == ".mdx")
        return std::make_unique<MdictDictionary>(main_file);
    if (ext == ".ifo")
        return std::make_unique<StardictDictionary>(main_file);
    throw std::runtime_error("不支持的词典格式: " + utf8(main_file.filename()));
}
} // namespace dr
