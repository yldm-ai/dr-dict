#include "dr/compress.h"
#include <miniz.h>
#include <stdexcept>
namespace dr
{
uint32_t adler32(std::string_view data)
{
    return static_cast<uint32_t>(
        mz_adler32(MZ_ADLER32_INIT, reinterpret_cast<const unsigned char *>(data.data()), data.size()));
}
std::string inflate_zlib(std::string_view data, size_t expected)
{
    std::string out(expected, '\0');
    auto n = tinfl_decompress_mem_to_mem(out.data(), out.size(), data.data(), data.size(),
                                         TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (n != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED && n == expected)
        return out;
    // Some writers record an inaccurate size; fall back to an unbounded inflate.
    size_t size = 0;
    void *heap = tinfl_decompress_mem_to_heap(data.data(), data.size(), &size, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!heap)
        throw std::runtime_error("zlib 数据块解压失败");
    std::string result(static_cast<const char *>(heap), size);
    mz_free(heap);
    return result;
}
static size_t gzip_header_size(std::string_view d, std::string_view *extra = nullptr)
{
    if (d.size() < 18 || static_cast<unsigned char>(d[0]) != 0x1f || static_cast<unsigned char>(d[1]) != 0x8b ||
        d[2] != 8)
        throw std::runtime_error("不是有效的 gzip 文件");
    const auto flags = static_cast<unsigned char>(d[3]);
    size_t p = 10;
    auto need = [&](size_t n) {
        if (p + n > d.size())
            throw std::runtime_error("gzip 文件头已损坏");
    };
    if (flags & 4)
    {
        need(2);
        size_t len = static_cast<unsigned char>(d[p]) | (static_cast<unsigned char>(d[p + 1]) << 8);
        p += 2;
        need(len);
        if (extra)
            *extra = d.substr(p, len);
        p += len;
    }
    for (int bit : {8, 16})
        if (flags & bit)
        {
            auto end = d.find('\0', p);
            if (end == std::string_view::npos)
                throw std::runtime_error("gzip 文件头已损坏");
            p = end + 1;
        }
    if (flags & 2)
        p += 2;
    need(0);
    return p;
}
std::string gunzip(std::string_view data)
{
    auto start = gzip_header_size(data);
    size_t size = 0;
    void *heap = tinfl_decompress_mem_to_heap(data.data() + start, data.size() - start, &size, 0);
    if (!heap)
        throw std::runtime_error("gzip 解压失败");
    std::string result(static_cast<const char *>(heap), size);
    mz_free(heap);
    return result;
}

// LZO1X stream layout as documented in the LZO distribution (doc/LZO.TXT, lzo1x_d.ch): a
// sequence of literal runs and back-references (M1..M4 instructions), terminated by an M4
// instruction with zero distance. Every read and copy below is checked against both buffers.
std::string lzo1x_decompress(std::string_view data, size_t expected)
{
    const auto *in = reinterpret_cast<const unsigned char *>(data.data());
    const size_t in_size = data.size();
    std::string out;
    out.reserve(expected);
    size_t ip = 0;
    auto fail = [] { throw std::runtime_error("LZO 数据块已损坏"); };
    auto byte = [&]() -> size_t {
        if (ip >= in_size)
            fail();
        return in[ip++];
    };
    auto literals = [&](size_t n) {
        if (ip + n > in_size)
            fail();
        out.append(reinterpret_cast<const char *>(in + ip), n);
        ip += n;
    };
    auto long_length = [&](size_t base) {
        size_t t = 0;
        while (true)
        {
            if (ip >= in_size)
                fail();
            if (in[ip] != 0)
                break;
            t += 255;
            ++ip;
            if (t > (size_t(1) << 31))
                fail();
        }
        return t + base + byte();
    };
    auto copy_match = [&](size_t distance, size_t length) {
        if (distance == 0 || distance > out.size())
            fail();
        size_t from = out.size() - distance;
        for (size_t i = 0; i < length; ++i)
            out.push_back(out[from + i]);
    };
    enum class State
    {
        Instruction,
        FirstLiteral,
        Match,
        TrailingLiterals
    };
    State state = State::Instruction;
    size_t t = 0;
    if (in_size > 0 && in[0] > 17)
    {
        t = byte() - 17;
        if (t < 4)
            state = State::TrailingLiterals;
        else
        {
            literals(t);
            state = State::FirstLiteral;
        }
    }
    while (true)
    {
        if (out.size() > expected + 16)
            fail();
        if (state == State::Instruction)
        {
            t = byte();
            if (t >= 16)
            {
                state = State::Match;
                continue;
            }
            literals(t == 0 ? long_length(15) + 3 : t + 3);
            state = State::FirstLiteral;
            continue;
        }
        if (state == State::FirstLiteral)
        {
            t = byte();
            if (t >= 16)
            {
                state = State::Match;
                continue;
            }
            // A short match directly after a literal run reaches past the 2 KiB M2 window.
            size_t distance = 1 + 0x0800 + (t >> 2);
            distance += byte() << 2;
            copy_match(distance, 3);
        }
        else if (state == State::TrailingLiterals)
        {
            literals(t);
            t = byte();
            state = State::Match;
            continue;
        }
        else
        {
            if (t >= 64)
            {
                size_t distance = 1 + ((t >> 2) & 7);
                distance += byte() << 3;
                copy_match(distance, (t >> 5) - 1 + 2);
            }
            else if (t >= 32)
            {
                size_t length = t & 31;
                if (length == 0)
                    length = long_length(31);
                size_t b0 = byte(), b1 = byte();
                copy_match(1 + (b0 >> 2) + (b1 << 6), length + 2);
            }
            else if (t >= 16)
            {
                size_t distance = (t & 8) << 11;
                size_t length = t & 7;
                if (length == 0)
                    length = long_length(7);
                size_t b0 = byte(), b1 = byte();
                distance += (b0 >> 2) + (b1 << 6);
                if (distance == 0)
                    break; // end of stream
                copy_match(distance + 0x4000, length + 2);
            }
            else
            {
                size_t distance = 1 + (t >> 2);
                distance += byte() << 2;
                copy_match(distance, 2);
            }
        }
        // The low two bits of the instruction's first distance byte encode 0-3 trailing literals.
        if (ip < 2)
            fail();
        t = in[ip - 2] & 3;
        if (t == 0)
            state = State::Instruction;
        else
            state = State::TrailingLiterals;
    }
    if (out.size() != expected)
        throw std::runtime_error("LZO 解压长度不符");
    return out;
}

BinaryFile::BinaryFile(const fs::path &path)
{
    if (_wfopen_s(&file_, path.c_str(), L"rb") != 0 || !file_)
        throw std::runtime_error("无法打开文件: " + utf8(path));
    _fseeki64(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(_ftelli64(file_));
}
BinaryFile::~BinaryFile()
{
    if (file_)
        fclose(file_);
}
std::string BinaryFile::read(uint64_t offset, size_t size)
{
    if (offset > size_ || size > size_ - offset)
        throw std::runtime_error("读取位置超出文件范围");
    std::string out(size, '\0');
    std::lock_guard lock(mutex_);
    if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0 ||
        (size && fread(out.data(), 1, size, file_) != size))
        throw std::runtime_error("读取文件失败");
    return out;
}

DictZip::DictZip(const fs::path &path) : file_(path)
{
    auto head = file_.read(0, static_cast<size_t>(std::min<uint64_t>(file_.size(), 1 << 20)));
    std::string_view extra;
    size_t start = gzip_header_size(head, &extra);
    // Extra subfields: SI1 SI2 LEN(2) DATA. The "RA" field carries VER, CHLEN, CHCNT, sizes...
    for (size_t p = 0; p + 4 <= extra.size();)
    {
        auto u16 = [&](size_t at) {
            return static_cast<unsigned>(static_cast<unsigned char>(extra[at]) |
                                         (static_cast<unsigned char>(extra[at + 1]) << 8));
        };
        size_t len = u16(p + 2);
        if (p + 4 + len > extra.size())
            break;
        if (extra[p] == 'R' && extra[p + 1] == 'A' && len >= 6)
        {
            auto field = p + 4;
            chunk_length_ = u16(field + 2);
            size_t count = u16(field + 4);
            if (6 + count * 2 <= len && chunk_length_ > 0)
            {
                uint64_t offset = start;
                for (size_t i = 0; i < count; ++i)
                {
                    chunk_offsets_.push_back(offset);
                    offset += u16(field + 6 + i * 2);
                }
                chunk_offsets_.push_back(offset);
                random_access_ = true;
            }
        }
        p += 4 + len;
    }
    if (!random_access_)
        whole_ = gunzip(file_.read(0, static_cast<size_t>(file_.size())));
}
std::string DictZip::chunk(size_t index)
{
    if (index == cached_index_)
        return cached_;
    if (index + 1 >= chunk_offsets_.size())
        throw std::runtime_error("dictzip 读取位置超出范围");
    auto compressed = file_.read(chunk_offsets_[index], static_cast<size_t>(chunk_offsets_[index + 1] - chunk_offsets_[index]));
    std::string out(chunk_length_, '\0');
    tinfl_decompressor inflator;
    tinfl_init(&inflator);
    size_t in_size = compressed.size(), out_size = out.size();
    auto status = tinfl_decompress(&inflator, reinterpret_cast<const mz_uint8 *>(compressed.data()), &in_size,
                                   reinterpret_cast<mz_uint8 *>(out.data()), reinterpret_cast<mz_uint8 *>(out.data()),
                                   &out_size, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT);
    if (status < TINFL_STATUS_DONE)
        throw std::runtime_error("dictzip 数据块解压失败");
    out.resize(out_size);
    cached_index_ = index;
    cached_ = out;
    return out;
}
std::string DictZip::read(uint64_t offset, size_t size)
{
    std::lock_guard lock(mutex_);
    if (!random_access_)
    {
        if (offset > whole_.size() || size > whole_.size() - offset)
            throw std::runtime_error("词条位置超出词典数据范围");
        return whole_.substr(static_cast<size_t>(offset), size);
    }
    std::string out;
    out.reserve(size);
    while (out.size() < size)
    {
        auto at = offset + out.size();
        auto index = static_cast<size_t>(at / chunk_length_);
        auto data = chunk(index);
        auto within = static_cast<size_t>(at % chunk_length_);
        if (within >= data.size())
            throw std::runtime_error("词条位置超出词典数据范围");
        out.append(data, within, std::min(size - out.size(), data.size() - within));
    }
    return out;
}
} // namespace dr
