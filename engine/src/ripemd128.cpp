#include "dr/ripemd128.h"
#include <cstring>
#include <vector>
namespace dr
{
namespace
{
constexpr uint8_t left_word[64] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                   7,  4,  13, 1,  10, 6,  15, 3,  12, 0,  9,  5,  2,  14, 11, 8,
                                   3,  10, 14, 4,  9,  15, 8,  1,  2,  7,  0,  6,  13, 11, 5,  12,
                                   1,  9,  11, 10, 0,  8,  12, 4,  13, 3,  7,  15, 14, 5,  6,  2};
constexpr uint8_t right_word[64] = {5,  14, 7,  0,  9, 2,  11, 4,  13, 6,  15, 8,  1,  10, 3,  12,
                                    6,  11, 3,  7,  0, 13, 5,  10, 14, 15, 8,  12, 4,  9,  1,  2,
                                    15, 5,  1,  3,  7, 14, 6,  9,  11, 8,  12, 2,  10, 0,  4,  13,
                                    8,  6,  4,  1,  3, 11, 15, 0,  5,  12, 2,  13, 9,  7,  10, 14};
constexpr uint8_t left_shift[64] = {11, 14, 15, 12, 5,  8,  7,  9,  11, 13, 14, 15, 6,  7,  9,  8,
                                    7,  6,  8,  13, 11, 9,  7,  15, 7,  12, 15, 9,  11, 7,  13, 12,
                                    11, 13, 6,  7,  14, 9,  13, 15, 14, 8,  13, 6,  5,  12, 7,  5,
                                    11, 12, 14, 15, 14, 15, 9,  8,  9,  14, 5,  6,  8,  6,  5,  12};
constexpr uint8_t right_shift[64] = {8,  9,  9,  11, 13, 15, 15, 5,  7,  7,  8,  11, 14, 14, 12, 6,
                                     9,  13, 15, 7,  12, 8,  9,  11, 7,  7,  12, 7,  6,  15, 13, 11,
                                     9,  7,  15, 11, 8,  6,  6,  14, 12, 13, 5,  14, 13, 13, 7,  5,
                                     15, 5,  8,  11, 14, 14, 6,  14, 6,  9,  12, 9,  12, 5,  15, 8};
constexpr uint32_t left_constant[4] = {0x00000000, 0x5a827999, 0x6ed9eba1, 0x8f1bbcdc};
constexpr uint32_t right_constant[4] = {0x50a28be6, 0x5c4dd124, 0x6d703ef3, 0x00000000};
inline uint32_t rotate(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}
inline uint32_t mix(int round, uint32_t x, uint32_t y, uint32_t z)
{
    switch (round)
    {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    default: return (x & z) | (y & ~z);
    }
}
void compress(uint32_t state[4], const uint8_t block[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; ++i)
        x[i] = block[i * 4] | (block[i * 4 + 1] << 8) | (block[i * 4 + 2] << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t ar = a, br = b, cr = c, dr = d;
    for (int j = 0; j < 64; ++j)
    {
        const int round = j / 16;
        uint32_t t = rotate(a + mix(round, b, c, d) + x[left_word[j]] + left_constant[round], left_shift[j]);
        a = d;
        d = c;
        c = b;
        b = t;
        // The parallel line applies the boolean functions in reverse order.
        t = rotate(ar + mix(3 - round, br, cr, dr) + x[right_word[j]] + right_constant[round], right_shift[j]);
        ar = dr;
        dr = cr;
        cr = br;
        br = t;
    }
    const uint32_t t = state[1] + c + dr;
    state[1] = state[2] + d + ar;
    state[2] = state[3] + a + br;
    state[3] = state[0] + b + cr;
    state[0] = t;
}
} // namespace
std::array<uint8_t, 16> ripemd128(std::string_view data)
{
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::vector<uint8_t> message(data.begin(), data.end());
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56)
        message.push_back(0);
    for (int i = 0; i < 8; ++i)
        message.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    for (size_t i = 0; i < message.size(); i += 64)
        compress(state, message.data() + i);
    std::array<uint8_t, 16> digest{};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            digest[i * 4 + k] = static_cast<uint8_t>(state[i] >> (8 * k));
    return digest;
}
} // namespace dr
