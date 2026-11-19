#include "dr/text.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <windows.h>
namespace dr
{
std::wstring widen(std::string_view s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}
std::string narrow(std::wstring_view s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::string lower_ascii(std::string_view s)
{
    std::string r(s);
    for (auto &c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return r;
}
std::string lower_text(std::string_view s)
{
    auto w = widen(s);
    if (!w.empty())
        CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return narrow(w);
}
static UINT codepage(std::string_view encoding)
{
    std::string e = lower_ascii(encoding);
    std::erase_if(e, [](char c) { return c == '-' || c == '_' || c == ' '; });
    if (e.empty() || e == "utf8")
        return CP_UTF8;
    if (e == "gbk" || e == "gb2312" || e == "gb18030" || e == "cp936")
        return 54936;
    if (e == "big5" || e == "big5hkscs" || e == "cp950")
        return 950;
    if (e == "shiftjis" || e == "sjis" || e == "cp932")
        return 932;
    if (e == "euckr" || e == "cp949")
        return 949;
    if (e == "eucjp")
        return 20932;
    if (e == "iso88591" || e == "latin1")
        return 28591;
    if (e.starts_with("windows") || e.starts_with("cp"))
    {
        auto digits = e.substr(e.starts_with("windows") ? 7 : 2);
        if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos)
            return static_cast<UINT>(std::stoul(digits));
    }
    return CP_UTF8;
}
std::string to_utf8(std::string_view bytes, std::string_view encoding)
{
    std::string e = lower_ascii(encoding);
    std::erase_if(e, [](char c) { return c == '-' || c == '_'; });
    if (e == "utf16" || e == "utf16le" || e == "ucs2")
    {
        std::wstring_view w(reinterpret_cast<const wchar_t *>(bytes.data()), bytes.size() / 2);
        while (!w.empty() && w.back() == 0)
            w.remove_suffix(1);
        return narrow(w);
    }
    auto cp = codepage(encoding);
    if (cp == CP_UTF8)
        return std::string(bytes);
    if (bytes.empty())
        return {};
    int n = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0)
        return std::string(bytes);
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    return narrow(w);
}
bool valid_utf8(std::string_view s)
{
    return s.empty() ||
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}
static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
std::string trim(std::string_view s)
{
    while (!s.empty() && (space(s.front()) || s.front() == '\0'))
        s.remove_prefix(1);
    while (!s.empty() && (space(s.back()) || s.back() == '\0'))
        s.remove_suffix(1);
    return std::string(s);
}
static std::wstring normalize(NORM_FORM form, const std::wstring &w)
{
    if (w.empty())
        return w;
    int estimate = NormalizeString(form, w.c_str(), static_cast<int>(w.size()), nullptr, 0);
    for (int attempt = 0; attempt < 4 && estimate > 0; ++attempt)
    {
        std::wstring out(estimate, 0);
        int n = NormalizeString(form, w.c_str(), static_cast<int>(w.size()), out.data(), estimate);
        if (n > 0)
        {
            out.resize(n);
            return out;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;
        estimate = -n * 2 + 16;
    }
    return w;
}
std::string fold_key(std::string_view text)
{
    auto w = normalize(NormalizationKD, widen(trim(text)));
    // Only Latin/Greek/Cyrillic combining marks are dropped; kana voicing marks must survive.
    std::erase_if(w, [](wchar_t c) { return c >= 0x0300 && c <= 0x036f; });
    if (!w.empty())
    {
        std::wstring lower(w.size() * 2 + 4, 0);
        int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), static_cast<int>(w.size()),
                              lower.data(), static_cast<int>(lower.size()), nullptr, nullptr, 0);
        if (n > 0)
        {
            lower.resize(n);
            w = std::move(lower);
        }
    }
    w = normalize(NormalizationC, w);
    std::wstring kept;
    kept.reserve(w.size());
    std::vector<WORD> types(w.size() + 1);
    if (!w.empty() && GetStringTypeW(CT_CTYPE1, w.c_str(), static_cast<int>(w.size()), types.data()))
    {
        for (size_t i = 0; i < w.size(); ++i)
        {
            const wchar_t c = w[i];
            const bool surrogate = c >= 0xd800 && c <= 0xdfff;
            if (surrogate || (types[i] & (C1_ALPHA | C1_DIGIT)) || (c >= 0x3040 && c <= 0x30ff) ||
                (c >= 0x3400 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af))
                kept.push_back(c);
        }
    }
    // Keys made only of punctuation (e.g. "+" or "?") still need to be found.
    if (kept.empty())
    {
        std::wstring compact;
        for (auto c : w)
            if (!iswspace(c))
                compact.push_back(c);
        kept = compact;
    }
    return narrow(kept);
}
std::string html_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}
static void append_code_point(std::string &out, uint32_t cp)
{
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    else if (cp < 0x110000)
    {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}
std::string decode_entities(std::string_view s)
{
    static const std::pair<std::string_view, std::string_view> named[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", "\xc2\xa0"}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out.push_back(s[i]);
            continue;
        }
        auto end = s.find(';', i);
        if (end == std::string_view::npos || end - i > 12)
        {
            out.push_back('&');
            continue;
        }
        auto name = s.substr(i + 1, end - i - 1);
        bool done = false;
        if (name.size() > 1 && name[0] == '#')
        {
            try
            {
                const bool hex = name[1] == 'x' || name[1] == 'X';
                auto digits = std::string(name.substr(hex ? 2 : 1));
                size_t used = 0;
                auto cp = std::stoul(digits, &used, hex ? 16 : 10);
                if (used == digits.size() && cp > 0)
                {
                    append_code_point(out, static_cast<uint32_t>(cp));
                    done = true;
                }
            }
            catch (const std::exception &)
            {
            }
        }
        else
            for (auto &[key, value] : named)
                if (name == key)
                {
                    out += value;
                    done = true;
                    break;
                }
        if (done)
            i = end;
        else
            out.push_back('&');
    }
    return out;
}
std::string strip_tags(std::string_view html)
{
    std::string out;
    bool tag = false;
    for (char c : html)
    {
        if (c == '<')
            tag = true;
        else if (c == '>')
            tag = false;
        else if (!tag)
            out.push_back(c);
    }
    return trim(decode_entities(out));
}
std::string article_text(std::string_view html)
{
    // Tags whose content is not text, and tags that end a line or block (a space stands for them,
    // so "<b>n.</b><br>apple" reads "n. apple" instead of "n.apple").
    static const std::string_view hidden[] = {"script", "style", "head", "title"};
    static const std::string_view breaking[] = {"br", "p", "div", "li", "ul", "ol", "tr", "td", "th", "table",
                                                "h1", "h2", "h3", "h4", "h5", "h6", "hr", "dd", "dt", "dl",
                                                "blockquote", "section"};
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size())
    {
        if (html[i] != '<')
        {
            out.push_back(html[i++]);
            continue;
        }
        const auto end = html.find('>', i);
        if (end == std::string_view::npos)
            break;
        auto tag = html.substr(i + 1, end - i - 1);
        const bool closing = !tag.empty() && tag.front() == '/';
        if (closing)
            tag.remove_prefix(1);
        size_t name_end = 0;
        while (name_end < tag.size() && std::isalnum(static_cast<unsigned char>(tag[name_end])))
            ++name_end;
        const auto name = lower_ascii(tag.substr(0, name_end));
        i = end + 1;
        if (!closing && std::find(std::begin(hidden), std::end(hidden), name) != std::end(hidden))
        {
            // Skip to the matching close tag; without one, drop the rest.
            const auto close = lower_ascii(html.substr(i)).find("</" + name);
            if (close == std::string::npos)
                break;
            const auto after = html.find('>', i + close);
            i = after == std::string_view::npos ? html.size() : after + 1;
            continue;
        }
        if (std::find(std::begin(breaking), std::end(breaking), name) != std::end(breaking))
            out.push_back(' ');
    }
    // Entities first, so &nbsp; also collapses into the single spaces below.
    const auto decoded = decode_entities(out);
    std::string text;
    text.reserve(decoded.size());
    bool space = false;
    for (size_t j = 0; j < decoded.size(); ++j)
    {
        const unsigned char c = static_cast<unsigned char>(decoded[j]);
        const bool nbsp = c == 0xc2 && j + 1 < decoded.size() && static_cast<unsigned char>(decoded[j + 1]) == 0xa0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || nbsp)
        {
            space = true;
            if (nbsp)
                ++j;
            continue;
        }
        if (space && !text.empty())
            text.push_back(' ');
        space = false;
        text.push_back(decoded[j]);
    }
    return text;
}
std::string truncate_text(std::string_view text, size_t max_chars)
{
    size_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        // Count code points: every byte that does not continue a UTF-8 sequence starts one.
        if ((static_cast<unsigned char>(text[i]) & 0xc0) == 0x80)
            continue;
        if (chars++ == max_chars)
            return trim(text.substr(0, i)) + "\xe2\x80\xa6"; // …
    }
    return std::string(text);
}
} // namespace dr
