#pragma once
#include "dr/compress.h"
#include "dr/dictionary.h"
#include <list>
#include <map>
namespace dr
{
// Reader for Octopus MDict 1.x/2.x .mdx and .mdd files. The layout follows the public format
// notes (https://github.com/zhansliu/writemdict/blob/master/fileformat.md); GoldenDict-ng's
// src/dict/mdictparser.cc was consulted for the order of sections, the GB18030/UTF-16LE encoding
// defaults and the key-index decryption, and the code here is an independent implementation.
class MdictFile
{
  public:
    explicit MdictFile(const fs::path &path);
    const std::map<std::string, std::string> &attributes() const { return attributes_; }
    std::string attribute(const std::string &name) const;
    // Encoding of headwords and articles; .mdd keys are always UTF-16LE.
    const std::string &encoding() const { return encoding_; }
    double version() const { return version_; }
    uint64_t entry_count() const { return entry_count_; }
    // Visits every key with its record offset in the concatenated (decompressed) record data.
    void keys(const std::function<void(std::string key, uint64_t offset)> &visit,
              const std::function<void(double)> &progress = {});
    // Size that a record starting at `offset` may span at most (to its block's end).
    uint64_t block_end(uint64_t offset) const;
    std::string record(uint64_t offset, uint64_t size);
    const fs::path &path() const { return path_; }

    static std::string decode_block(std::string_view raw, uint64_t decompressed_size);
    static void decrypt_key_index(std::string &block);

  private:
    struct KeyBlock
    {
        uint64_t compressed, decompressed;
    };
    struct RecordBlock
    {
        uint64_t file_offset, compressed, data_offset, decompressed;
    };
    std::string block(size_t index);
    fs::path path_;
    BinaryFile file_;
    std::map<std::string, std::string> attributes_;
    std::string encoding_;
    double version_ = 0;
    int number_size_ = 4;
    int encrypted_ = 0;
    uint64_t entry_count_ = 0;
    uint64_t key_blocks_offset_ = 0;
    std::vector<KeyBlock> key_blocks_;
    std::vector<RecordBlock> records_;
    std::mutex cache_mutex_;
    std::list<std::pair<size_t, std::string>> cache_;
};

std::map<std::string, std::string> parse_header_attributes(std::string_view header);
// Expands MDict "`n`" style markers using the header's StyleSheet triples.
std::string apply_mdict_styles(std::string_view article,
                               const std::map<int, std::pair<std::string, std::string>> &styles);
std::map<int, std::pair<std::string, std::string>> parse_mdict_styles(std::string_view sheet);

class MdictDictionary final : public Dictionary
{
  public:
    explicit MdictDictionary(const fs::path &mdx);
    std::string format() const override { return "mdict"; }
    std::string title() const override { return title_; }
    std::string description() const override;
    void index(const IndexSink &sink) override;
    std::string article(uint64_t a, uint64_t b) override;
    std::string resource(int volume, uint64_t a, uint64_t b) override;
    fs::path resource_folder() const override { return mdx_path_.parent_path(); }
    std::optional<fs::path> icon() const override;
    std::vector<fs::path> files() const override;

  private:
    fs::path mdx_path_;
    MdictFile mdx_;
    std::vector<std::unique_ptr<MdictFile>> mdd_;
    std::string title_;
    std::map<int, std::pair<std::string, std::string>> styles_;
};
} // namespace dr
