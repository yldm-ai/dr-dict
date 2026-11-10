#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace dr
{
// RIPEMD-128 (Dobbertin, Bosselaers, Preneel, 1996). MDict derives its key-index decryption key
// from this digest.
std::array<uint8_t, 16> ripemd128(std::string_view data);
} // namespace dr
