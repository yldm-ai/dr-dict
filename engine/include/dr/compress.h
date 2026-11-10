#pragma once
#include "dr/paths.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
namespace dr
{
uint32_t adler32(std::string_view data);
// zlib stream (RFC 1950) with a known decompressed size.
std::string inflate_zlib(std::string_view data, size_t expected);
// Whole gzip member (RFC 1952) of unknown size.
std::string gunzip(std::string_view data);
// LZO1X decompression (independent, bounds-checked implementation of the documented format).
std::string lzo1x_decompress(std::string_view data, size_t expected);

// Random-access reader for a plain file opened once and read at offsets.
class BinaryFile
{
  public:
    explicit BinaryFile(const fs::path &path);
    ~BinaryFile();
    BinaryFile(const BinaryFile &) = delete;
    BinaryFile &operator=(const BinaryFile &) = delete;
    std::string read(uint64_t offset, size_t size);
    uint64_t size() const { return size_; }

  private:
    std::mutex mutex_;
    FILE *file_ = nullptr;
    uint64_t size_ = 0;
};

// Reads a dictzip (.dz) file. The "RA" extra field lists independently flushed deflate chunks
// (see the dictzip(1) format description); files without it are inflated once into memory.
class DictZip
{
  public:
    explicit DictZip(const fs::path &path);
    std::string read(uint64_t offset, size_t size);

  private:
    std::string chunk(size_t index);
    BinaryFile file_;
    uint32_t chunk_length_ = 0;
    std::vector<uint64_t> chunk_offsets_; // compressed offsets, one past the last as sentinel
    std::string whole_;
    bool random_access_ = false;
    std::mutex mutex_;
    size_t cached_index_ = SIZE_MAX;
    std::string cached_;
};
} // namespace dr
