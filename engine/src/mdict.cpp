#include "dr/mdict.h"
#include "dr/ripemd128.h"
#include "dr/text.h"
#include <algorithm>
#include <stdexcept>
namespace dr
{
namespace
{
uint64_t big_endian(std::string_view s, size_t at, int bytes)
{
    if (at + bytes > s.size())
        throw std::runtime_error("MDict 文件结构已损坏");
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v = (v << 8) | static_cast<unsigned char>(s[at + i]);
    return v;
}
struct Cursor
{
    std::string_view data;
    size_t at = 0;
    uint64_t number(int bytes)
    {
        auto v = big_endian(data, at, bytes);
        at += bytes;
        return v;
    }
    void skip(size_t n)
    {
        if (at + n > data.size())
            throw std::runtime_error("MDict 索引结构已损坏");
        at += n;
    }
    bool done() const { return at >= data.size(); }
};
constexpr size_t max_section = size_t(1) << 31;
} // namespace

std::map<std::string, std::string> parse_header_attributes(std::string_view h)
{
    // The header is a single XML element, but real files contain control characters and
    // unescaped markup inside values, so attributes are scanned instead of parsed as XML.
    std::map<std::string, std::string> result;
    size_t p = h.find('<');
    if (p == std::string_view::npos)
        return result;
    p = h.find_first_of(" \t\r\n", p);
    while (p != std::string_view::npos && p < h.size())
    {
        p = h.find_first_not_of(" \t\r\n", p);
        if (p == std::string_view::npos || h[p] == '/' || h[p] == '>')
            break;
        auto eq = h.find('=', p);
        if (eq == std::string_view::npos)
            break;
        auto name = trim(h.substr(p, eq - p));
        auto quote = h.find_first_of("\"'", eq);
        if (quote == std::string_view::npos)
            break;
        auto end = h.find(h[quote], quote + 1);
        if (end == std::string_view::npos)
            end = h.size();
        result[name] = decode_entities(h.substr(quote + 1, end - quote - 1));
        p = end + 1;
    }
    return result;
}

std::map<int, std::pair<std::string, std::string>> parse_mdict_styles(std::string_view sheet)
{
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= sheet.size(); ++i)
        if (i == sheet.size() || sheet[i] == '\n' || sheet[i] == '\r')
        {
            if (i < sheet.size() && sheet[i] == '\r' && i + 1 < sheet.size() && sheet[i + 1] == '\n')
            {
                lines.emplace_back(sheet.substr(start, i - start));
                start = i + 2;
                ++i;
                continue;
            }
            lines.emplace_back(sheet.substr(start, i - start));
            start = i + 1;
        }
    std::map<int, std::pair<std::string, std::string>> styles;
    for (size_t i = 0; i + 2 < lines.size(); i += 3)
    {
        auto id = trim(lines[i]);
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos || id.size() > 4)
            continue;
        styles[std::stoi(id)] = {decode_entities(lines[i + 1]), decode_entities(lines[i + 2])};
    }
    return styles;
}

std::string apply_mdict_styles(std::string_view article,
                               const std::map<int, std::pair<std::string, std::string>> &styles)
{
    if (styles.empty())
        return std::string(article);
    std::string out, closing;
    out.reserve(article.size() + 64);
    for (size_t i = 0; i < article.size(); ++i)
    {
        if (article[i] == '`')
        {
            size_t j = i + 1;
            while (j < article.size() && j - i <= 4 && article[j] >= '0' && article[j] <= '9')
                ++j;
            if (j > i + 1 && j < article.size() && article[j] == '`')
            {
                auto it = styles.find(std::stoi(std::string(article.substr(i + 1, j - i - 1))));
                out += closing;
                if (it != styles.end())
                {
                    out += it->second.first;
                    closing = it->second.second;
                }
                else
                    closing.clear();
                i = j;
                continue;
            }
        }
        out.push_back(article[i]);
    }
    return out + closing;
}

MdictFile::MdictFile(const fs::path &path) : path_(path), file_(path)
{
    auto header_size = big_endian(file_.read(0, 4), 0, 4);
    if (header_size == 0 || header_size > (1 << 24))
        throw std::runtime_error("不是有效的 MDict 文件");
    auto header = file_.read(4, static_cast<size_t>(header_size));
    attributes_ = parse_header_attributes(to_utf8(header, "UTF-16LE"));
    if (attributes_.empty())
        throw std::runtime_error("无法解析 MDict 文件头");
    uint64_t pos = 4 + header_size + 4; // the trailing adler32 is little-endian
    const bool mdd = lower_ascii(path.extension().string()) == ".mdd";
    encoding_ = attribute("Encoding");
    auto lowered = lower_ascii(encoding_);
    if (mdd || encoding_.empty() || lowered == "utf-16" || lowered == "utf16")
        encoding_ = "UTF-16LE";
    else if (lowered == "gbk" || lowered == "gb2312")
        encoding_ = "GB18030";
    try
    {
        version_ = std::stod(attribute("GeneratedByEngineVersion"));
    }
    catch (const std::exception &)
    {
        version_ = 1.2;
    }
    if (version_ >= 3.0)
        throw std::runtime_error("暂不支持 MDict 3.0 格式");
    number_size_ = version_ < 2.0 ? 4 : 8;
    auto encrypted = lower_ascii(trim(attribute("Encrypted")));
    if (encrypted == "yes")
        encrypted_ = 1;
    else if (!encrypted.empty() && encrypted.find_first_not_of("0123456789") == std::string::npos)
        encrypted_ = std::stoi(encrypted);
    if (encrypted_ & 1)
        throw std::runtime_error("该词典需要注册码解密，暂不支持");

    // Key section header.
    const size_t key_header_size = version_ >= 2.0 ? 8 * 5 : 4 * 4;
    const auto key_header = file_.read(pos, key_header_size);
    Cursor kh{key_header};
    uint64_t block_count = kh.number(number_size_);
    entry_count_ = kh.number(number_size_);
    uint64_t info_decompressed = version_ >= 2.0 ? kh.number(8) : 0;
    uint64_t info_size = kh.number(number_size_);
    uint64_t key_blocks_size = kh.number(number_size_);
    pos += key_header_size + (version_ >= 2.0 ? 4 : 0);
    if (info_size > max_section || info_decompressed > max_section || block_count > max_section)
        throw std::runtime_error("MDict 索引过大或已损坏");
    auto info = file_.read(pos, static_cast<size_t>(info_size));
    pos += info_size;
    if (version_ >= 2.0)
    {
        if (encrypted_ & 2)
            decrypt_key_index(info);
        info = decode_block(info, info_decompressed);
    }
    const bool utf16 = encoding_ == "UTF-16LE";
    const size_t unit = utf16 ? 2 : 1;
    const size_t terminator = version_ >= 2.0 ? 1 : 0;
    Cursor ki{info};
    while (!ki.done() && key_blocks_.size() < block_count)
    {
        ki.skip(number_size_);
        for (int edge = 0; edge < 2; ++edge)
        {
            auto length = ki.number(version_ >= 2.0 ? 2 : 1);
            ki.skip(static_cast<size_t>((length + terminator) * unit));
        }
        KeyBlock b{};
        b.compressed = ki.number(number_size_);
        b.decompressed = ki.number(number_size_);
        key_blocks_.push_back(b);
    }
    key_blocks_offset_ = pos;
    pos += key_blocks_size;

    // Record section header and block table.
    const auto record_header = file_.read(pos, number_size_ * 4);
    Cursor rh{record_header};
    uint64_t record_blocks = rh.number(number_size_);
    rh.number(number_size_);
    uint64_t record_info_size = rh.number(number_size_);
    rh.number(number_size_);
    pos += number_size_ * 4;
    if (record_info_size > max_section || record_blocks * number_size_ * 2 > record_info_size + 16)
        throw std::runtime_error("MDict 记录表已损坏");
    auto table = file_.read(pos, static_cast<size_t>(record_info_size));
    pos += record_info_size;
    Cursor rt{table};
    uint64_t data_offset = 0, file_offset = pos;
    records_.reserve(static_cast<size_t>(record_blocks));
    for (uint64_t i = 0; i < record_blocks; ++i)
    {
        RecordBlock r{};
        r.compressed = rt.number(number_size_);
        r.decompressed = rt.number(number_size_);
        r.file_offset = file_offset;
        r.data_offset = data_offset;
        file_offset += r.compressed;
        data_offset += r.decompressed;
        records_.push_back(r);
    }
    if (file_offset > file_.size())
        throw std::runtime_error("MDict 文件不完整");
}

std::string MdictFile::attribute(const std::string &name) const
{
    auto it = attributes_.find(name);
    return it == attributes_.end() ? std::string() : it->second;
}

std::string MdictFile::decode_block(std::string_view raw, uint64_t decompressed_size)
{
    if (raw.size() < 8)
        throw std::runtime_error("MDict 数据块已损坏");
    // The 4-byte type tag is little-endian: 0 = stored, 1 = LZO, 2 = zlib.
    const auto type = static_cast<unsigned char>(raw[0]);
    auto body = raw.substr(8);
    switch (type)
    {
    case 0: return std::string(body);
    case 1: return lzo1x_decompress(body, static_cast<size_t>(decompressed_size));
    case 2: return inflate_zlib(body, static_cast<size_t>(decompressed_size));
    default: throw std::runtime_error("未知的 MDict 压缩类型");
    }
}

void MdictFile::decrypt_key_index(std::string &block)
{
    if (block.size() < 8)
        return;
    std::string seed = block.substr(4, 4) + std::string("\x95\x36\x00\x00", 4);
    auto key = ripemd128(seed);
    uint8_t previous = 0x36;
    for (size_t i = 8; i < block.size(); ++i)
    {
        const auto encrypted = static_cast<uint8_t>(block[i]);
        uint8_t b = static_cast<uint8_t>((encrypted >> 4) | (encrypted << 4));
        b = static_cast<uint8_t>(b ^ previous ^ ((i - 8) & 0xff) ^ key[(i - 8) % 16]);
        previous = encrypted;
        block[i] = static_cast<char>(b);
    }
}

void MdictFile::keys(const std::function<void(std::string, uint64_t)> &visit,
                     const std::function<void(double)> &progress)
{
    const bool utf16 = encoding_ == "UTF-16LE";
    uint64_t offset = key_blocks_offset_;
    for (size_t index = 0; index < key_blocks_.size(); ++index)
    {
        const auto &kb = key_blocks_[index];
        if (kb.compressed > max_section || kb.decompressed > max_section)
            throw std::runtime_error("MDict 词头块已损坏");
        auto block = decode_block(file_.read(offset, static_cast<size_t>(kb.compressed)), kb.decompressed);
        offset += kb.compressed;
        size_t p = 0;
        while (p + number_size_ <= block.size())
        {
            auto record = big_endian(block, p, number_size_);
            p += number_size_;
            size_t end = p;
            if (utf16)
            {
                while (end + 1 < block.size() && (block[end] != 0 || block[end + 1] != 0))
                    end += 2;
                visit(trim(to_utf8(std::string_view(block).substr(p, end - p), encoding_)), record);
                p = std::min(block.size(), end + 2);
            }
            else
            {
                while (end < block.size() && block[end] != 0)
                    ++end;
                visit(trim(to_utf8(std::string_view(block).substr(p, end - p), encoding_)), record);
                p = end + 1;
            }
        }
        if (progress && (index % 16 == 0 || index + 1 == key_blocks_.size()))
            progress(double(index + 1) / double(key_blocks_.size()));
    }
}

uint64_t MdictFile::block_end(uint64_t offset) const
{
    auto it = std::upper_bound(records_.begin(), records_.end(), offset,
                               [](uint64_t v, const RecordBlock &r) { return v < r.data_offset; });
    if (it == records_.begin())
        return offset;
    --it;
    return it->data_offset + it->decompressed;
}

std::string MdictFile::block(size_t index)
{
    {
        std::lock_guard lock(cache_mutex_);
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->first == index)
            {
                cache_.splice(cache_.begin(), cache_, it);
                return cache_.front().second;
            }
    }
    const auto &r = records_[index];
    if (r.compressed > max_section || r.decompressed > max_section)
        throw std::runtime_error("MDict 记录块已损坏");
    auto data = decode_block(file_.read(r.file_offset, static_cast<size_t>(r.compressed)), r.decompressed);
    std::lock_guard lock(cache_mutex_);
    cache_.emplace_front(index, data);
    if (cache_.size() > 6)
        cache_.pop_back();
    return data;
}

std::string MdictFile::record(uint64_t offset, uint64_t size)
{
    std::string out;
    auto it = std::upper_bound(records_.begin(), records_.end(), offset,
                               [](uint64_t v, const RecordBlock &r) { return v < r.data_offset; });
    if (it == records_.begin())
        throw std::runtime_error("词条位置无效");
    size_t index = static_cast<size_t>(it - records_.begin() - 1);
    uint64_t at = offset;
    while (out.size() < size && index < records_.size())
    {
        const auto &r = records_[index];
        auto data = block(index);
        if (at < r.data_offset || at - r.data_offset >= data.size())
            break;
        auto within = static_cast<size_t>(at - r.data_offset);
        auto take = std::min<uint64_t>(size - out.size(), data.size() - within);
        out.append(data, within, static_cast<size_t>(take));
        at += take;
        ++index;
    }
    return out;
}

MdictDictionary::MdictDictionary(const fs::path &mdx) : mdx_path_(mdx), mdx_(mdx)
{
    auto title = trim(mdx_.attribute("Title"));
    if (title.find('<') != std::string::npos)
        title = strip_tags(title);
    if (title.empty() || title == "Title (No HTML code allowed)")
        title = utf8(mdx.stem());
    title_ = title;
    styles_ = parse_mdict_styles(mdx_.attribute("StyleSheet"));
    // Resource volumes: name.mdd, name.1.mdd, name.2.mdd, …
    auto base = mdx;
    base.replace_extension();
    auto first = fs::path(base).concat(".mdd");
    std::error_code ec;
    if (fs::exists(first, ec))
    {
        mdd_.push_back(std::make_unique<MdictFile>(first));
        for (int volume = 1;; ++volume)
        {
            auto next = fs::path(base).concat("." + std::to_string(volume) + ".mdd");
            if (!fs::exists(next, ec))
                break;
            mdd_.push_back(std::make_unique<MdictFile>(next));
        }
    }
}

std::string MdictDictionary::description() const
{
    return apply_mdict_styles(mdx_.attribute("Description"), styles_);
}

void MdictDictionary::index(const IndexSink &sink)
{
    // Records follow key order, so a key's size is the distance to the next key's offset.
    uint64_t total = 1 + mdx_.entry_count();
    for (auto &mdd : mdd_)
        total += mdd->entry_count();
    uint64_t seen = 0;
    auto emit_all = [&](MdictFile &file, const std::function<void(const std::string &, uint64_t, uint64_t)> &emit) {
        std::string pending;
        uint64_t pending_offset = 0;
        bool has = false;
        auto flush = [&](uint64_t next) {
            uint64_t end = file.block_end(pending_offset);
            uint64_t size = next > pending_offset && next <= end ? next - pending_offset : end - pending_offset;
            emit(pending, pending_offset, size);
        };
        file.keys([&](std::string key, uint64_t offset) {
            if (has)
                flush(offset);
            pending = std::move(key);
            pending_offset = offset;
            has = true;
            if (sink.progress && (++seen % 4096 == 0))
                sink.progress(double(seen) / double(total));
        });
        if (has)
            flush(UINT64_MAX);
    };
    emit_all(mdx_, [&](const std::string &word, uint64_t a, uint64_t b) {
        if (!word.empty() && sink.word)
            sink.word(word, a, b);
    });
    for (size_t volume = 0; volume < mdd_.size(); ++volume)
        emit_all(*mdd_[volume], [&](const std::string &key, uint64_t a, uint64_t b) {
            if (key.empty() || !sink.resource)
                return;
            std::string path = key;
            std::replace(path.begin(), path.end(), '\\', '/');
            while (!path.empty() && path.front() == '/')
                path.erase(path.begin());
            sink.resource(path, static_cast<int>(volume), a, b);
        });
    if (sink.progress)
        sink.progress(1.0);
}

std::string MdictDictionary::article(uint64_t a, uint64_t b)
{
    auto text = trim(to_utf8(mdx_.record(a, b), mdx_.encoding()));
    if (text.starts_with("@@@LINK="))
        return text;
    return apply_mdict_styles(text, styles_);
}

std::string MdictDictionary::resource(int volume, uint64_t a, uint64_t b)
{
    if (volume < 0 || static_cast<size_t>(volume) >= mdd_.size())
        return {};
    return mdd_[volume]->record(a, b);
}

std::optional<fs::path> MdictDictionary::icon() const
{
    std::error_code ec;
    for (auto ext : {".png", ".jpg", ".jpeg", ".ico", ".bmp", ".gif", ".svg"})
    {
        auto candidate = mdx_path_;
        candidate.replace_extension(ext);
        if (fs::is_regular_file(candidate, ec))
            return candidate;
    }
    return std::nullopt;
}

std::vector<fs::path> MdictDictionary::files() const
{
    std::vector<fs::path> result{mdx_path_};
    for (auto &mdd : mdd_)
        result.push_back(mdd->path());
    return result;
}
} // namespace dr
