#include "dr/paths.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace dr
{
std::string utf8(const fs::path &p)
{
    auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
fs::path path8(std::string_view s)
{
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
std::string read_file(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("无法读取文件: " + utf8(p));
    return {std::istreambuf_iterator<char>(f), {}};
}
void write_file(const fs::path &p, std::string_view data)
{
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(data.data(), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("无法写入文件: " + utf8(p));
}
void write_atomic(const fs::path &p, std::string_view data)
{
    auto temp = p;
    temp += ".tmp";
    write_file(temp, data);
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("无法保存文件: " + utf8(p));
#else
    fs::rename(temp, p);
#endif
}
static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
std::string url_decode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
            i += 2;
        }
        else
            out.push_back(s[i]);
    }
    return out;
}
std::string url_encode(std::string_view s)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else
        {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 15]);
        }
    }
    return out;
}
} // namespace dr
