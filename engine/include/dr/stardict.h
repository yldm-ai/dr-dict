#pragma once
#include "dr/compress.h"
#include "dr/dictionary.h"
#include <map>
namespace dr
{
// StarDict 2.4.2+ dictionaries (.ifo/.idx[.gz]/.dict[.dz]/.syn) as described in the StarDict
// "StarDictFileFormat" document. GoldenDict-ng's src/dict/stardict.cc was consulted for how
// each sametypesequence field type is presented; the conversion here is independent.
class StardictDictionary final : public Dictionary
{
  public:
    explicit StardictDictionary(const fs::path &ifo);
    std::string format() const override { return "stardict"; }
    std::string title() const override;
    std::string description() const override;
    void index(const IndexSink &sink) override;
    std::string article(uint64_t a, uint64_t b) override;
    fs::path resource_folder() const override { return ifo_.parent_path() / "res"; }
    std::optional<fs::path> icon() const override;
    std::vector<fs::path> files() const override;
    const std::map<std::string, std::string> &info() const { return info_; }

  private:
    fs::path ifo_, idx_, dict_, syn_;
    std::map<std::string, std::string> info_;
    std::unique_ptr<BinaryFile> plain_;
    std::unique_ptr<DictZip> zipped_;
};
// Converts one record of the given types (sametypesequence, or empty for typed records) to HTML.
std::string stardict_to_html(std::string_view record, std::string_view sametypesequence);
} // namespace dr
