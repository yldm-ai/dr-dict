#include "dr/chinese.h"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <SimpleConverter.hpp> // OpenCC's build interface exposes its src/ folder directly
namespace dr
{
namespace
{
// traditional (any region) → simplified, applied before converting to the selected standard.
constexpr const char *to_simplified[] = {"t2s", "tw2s", "hk2s"};

std::vector<std::string> targets(const std::string &variant)
{
    if (variant == "t")
        return {"s2t"};
    if (variant == "tw")
        return {"s2tw"};
    if (variant == "twp")
        return {"s2twp"};
    if (variant == "hk")
        return {"s2hk"};
    return {"s2t", "s2tw", "s2twp", "s2hk"};
}
} // namespace

bool valid_chinese_variant(const std::string &variant)
{
    return variant == "t" || variant == "tw" || variant == "twp" || variant == "hk" || variant == "all";
}

bool has_han(const std::string &text)
{
    for (size_t i = 0; i < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        int length = 1;
        if (c < 0x80)
            cp = c;
        else if ((c >> 5) == 0x6)
            cp = c & 0x1f, length = 2;
        else if ((c >> 4) == 0xe)
            cp = c & 0x0f, length = 3;
        else if ((c >> 3) == 0x1e)
            cp = c & 0x07, length = 4;
        if (i + length > text.size())
            break;
        for (int k = 1; k < length; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        i += length;
        if ((cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0x20000 && cp <= 0x3134f))
            return true;
    }
    return false;
}

ChineseVariants::ChineseVariants(fs::path config_dir) : dir_(std::move(config_dir)) {}
ChineseVariants::~ChineseVariants() = default;

bool ChineseVariants::available() const
{
    std::error_code ec;
    return fs::is_regular_file(dir_ / "s2t.json", ec);
}

std::string ChineseVariants::convert(const std::string &config, const std::string &text)
{
    opencc::SimpleConverter *converter = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (failed_.count(config))
            return text;
        auto &slot = converters_[config];
        if (!slot)
        {
            try
            {
                // OpenCC resolves the dictionaries a config names relative to the config file.
                slot = std::make_unique<opencc::SimpleConverter>(utf8(dir_ / (config + ".json")));
            }
            catch (const std::exception &)
            {
                converters_.erase(config);
                failed_.insert(config);
                return text;
            }
        }
        converter = slot.get();
    }
    // A loaded converter is immutable; Convert() is const and may run on several threads.
    try
    {
        return converter->Convert(text);
    }
    catch (const std::exception &)
    {
        return text;
    }
}

std::string ChineseVariants::convert_html(const std::string &html, const std::string &script,
                                          const std::string &variant)
{
    std::string config;
    if (script == "s")
        config = "t2s";
    else if (script == "t")
        config = variant == "tw" ? "s2tw" : variant == "twp" ? "s2twp" : variant == "hk" ? "s2hk" : "s2t";
    else
        return html;
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    const auto find_ci = [&](size_t from, std::string_view needle) {
        for (size_t i = from; i + needle.size() <= html.size(); ++i)
        {
            size_t k = 0;
            while (k < needle.size() && lower(html[i + k]) == needle[k])
                ++k;
            if (k == needle.size())
                return i;
        }
        return std::string::npos;
    };
    std::string out;
    out.reserve(html.size() + html.size() / 8);
    size_t i = 0;
    while (i < html.size())
    {
        auto lt = html.find('<', i);
        // Text up to the next markup.
        const auto text_end = lt == std::string::npos ? html.size() : lt;
        if (text_end > i)
        {
            auto text = html.substr(i, text_end - i);
            out += has_han(text) ? convert(config, text) : text;
        }
        if (lt == std::string::npos)
            break;
        i = lt;
        if (html.compare(i, 4, "<!--") == 0)
        {
            auto end = html.find("-->", i + 4);
            end = end == std::string::npos ? html.size() : end + 3;
            out.append(html, i, end - i);
            i = end;
            continue;
        }
        const char next = i + 1 < html.size() ? html[i + 1] : '\0';
        if (!(std::isalpha(static_cast<unsigned char>(next)) || next == '/' || next == '!' || next == '?'))
        {
            out += '<'; // a literal "<" in text
            ++i;
            continue;
        }
        // The tag itself, with quoted attribute values that may contain '>'.
        size_t j = i + 1;
        char quote = 0;
        for (; j < html.size(); ++j)
        {
            const char c = html[j];
            if (quote)
                quote = c == quote ? 0 : quote;
            else if (c == '"' || c == '\'')
                quote = c;
            else if (c == '>')
                break;
        }
        const auto tag_end = j < html.size() ? j + 1 : html.size();
        std::string name;
        for (size_t k = i + 1; k < tag_end && std::isalnum(static_cast<unsigned char>(html[k])); ++k)
            name += lower(html[k]);
        out.append(html, i, tag_end - i);
        i = tag_end;
        if (name == "script" || name == "style")
        {
            auto close = find_ci(i, "</" + name);
            close = close == std::string::npos ? html.size() : close;
            out.append(html, i, close - i);
            i = close;
        }
    }
    return out;
}

std::vector<std::string> ChineseVariants::expand(const std::string &text, const std::string &variant)
{
    std::vector<std::string> result;
    if (!valid_chinese_variant(variant) || !has_han(text) || text.size() > 256)
        return result;
    auto add = [&](std::string value) {
        if (!value.empty() && value != text && std::find(result.begin(), result.end(), value) == result.end())
            result.push_back(std::move(value));
    };
    // Simplified forms first: the query itself may already be simplified.
    std::vector<std::string> simplified = {text};
    for (auto *config : to_simplified)
    {
        auto value = convert(config, text);
        if (std::find(simplified.begin(), simplified.end(), value) == simplified.end())
            simplified.push_back(value);
    }
    if (variant == "twp" || variant == "all")
    {
        // Taiwan vocabulary back to mainland vocabulary (軟體 → 软件).
        auto value = convert("tw2sp", text);
        if (std::find(simplified.begin(), simplified.end(), value) == simplified.end())
            simplified.push_back(value);
    }
    for (auto &s : simplified)
        add(s);
    for (auto &s : simplified)
        for (auto &config : targets(variant))
            add(convert(config, s));
    return result;
}
} // namespace dr
