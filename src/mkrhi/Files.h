#pragma once

#include <string>

// Whole-file reads/writes with UTF-8 paths (pipeline cache). Writes go to a temp file that replaces the target
namespace mkrhi::detail {

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
bool createDirectories(const std::string& path);

} // namespace mkrhi::detail
