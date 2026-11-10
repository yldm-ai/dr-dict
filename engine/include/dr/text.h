#pragma once
#include <string>
#include <string_view>
namespace dr
{
std::wstring widen(std::string_view text);
std::string narrow(std::wstring_view text);
// Converts dictionary bytes in a declared legacy encoding (UTF-8, UTF-16LE, GB18030, Big5, …) to UTF-8.
std::string to_utf8(std::string_view bytes, std::string_view encoding);
bool valid_utf8(std::string_view text);
std::string trim(std::string_view text);
// Index key used for case-, accent- and punctuation-insensitive lookup. Mirrors the idea of
// GoldenDict's Folding::apply (fold case, drop diacritics, ignore punctuation/whitespace), but is
// implemented with the Windows normalization and casing APIs.
std::string fold_key(std::string_view utf8);
std::string html_escape(std::string_view text);
std::string decode_entities(std::string_view text);
std::string strip_tags(std::string_view html);
std::string lower_ascii(std::string_view text);
// Unicode-aware lowercase used for case-insensitive resource names.
std::string lower_text(std::string_view text);
} // namespace dr
