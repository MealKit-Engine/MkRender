#include "mkrhi/Files.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace mkrhi::detail {
namespace {

std::filesystem::path toPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

} // namespace

bool readFile(const std::string& path, std::string& out) {
    out.clear();
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(out.data(), size);
    return static_cast<bool>(in);
}

bool writeFile(const std::string& path, const std::string& data) {
    const std::filesystem::path target = toPath(path);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream o(temp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!o) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) std::filesystem::remove(temp, ec);
    return !ec;
}

bool createDirectories(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(toPath(path), ec);
    return !ec;
}

} // namespace mkrhi::detail
