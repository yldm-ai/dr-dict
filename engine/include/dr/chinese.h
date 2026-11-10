#pragma once
#include "dr/paths.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
namespace opencc
{
class SimpleConverter;
}
namespace dr
{
// Which traditional standard a simplified query is also written in.
//   "t"   OpenCC standard traditional characters
//   "tw"  Taiwan standard characters
//   "twp" Taiwan standard with Taiwan vocabulary (软件 → 軟體)
//   "hk"  Hong Kong standard characters
//   "all" every one of the above
bool valid_chinese_variant(const std::string &variant);

// Simplified/traditional writings of a Chinese query, produced with OpenCC.
//
// Reference: GoldenDict-ng src/dict/transliteration/chinese.cc (CharacterConversionDictionary)
// looks a word up under the outputs of several OpenCC chains, so the user need not know which
// variant they typed. This independent implementation first brings the query to simplified
// with every traditional→simplified config (so traditional input of any region works), then
// converts each simplified form to the selected traditional standard(s).
class ChineseVariants
{
  public:
    /** `config_dir` holds OpenCC's *.json configs and the *.ocd2 dictionaries they name. */
    explicit ChineseVariants(fs::path config_dir);
    ~ChineseVariants();
    ChineseVariants(const ChineseVariants &) = delete;
    ChineseVariants &operator=(const ChineseVariants &) = delete;

    /** Other writings of `text` (never `text` itself), or none when it holds no Han characters. */
    std::vector<std::string> expand(const std::string &text, const std::string &variant);
    /** Whether OpenCC data is present at all. */
    bool available() const;

  private:
    std::string convert(const std::string &config, const std::string &text);

    fs::path dir_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<opencc::SimpleConverter>> converters_;
    std::set<std::string> failed_;
};

/** Whether UTF-8 `text` contains a CJK unified ideograph (including extensions and compatibility forms). */
bool has_han(const std::string &text);
} // namespace dr
