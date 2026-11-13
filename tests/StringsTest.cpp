// Every tr("...")/trf("...") text and panel title in tool/*.cpp has its Korean line in tool/Strings.cpp
// (engine docs/TOOL_DESIGN.md section 4: every visible string goes through tr() and has its Korean line)
#include "Strings.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

// Reads a C string literal starting at the opening quote; n and t escapes become newline and tab, others the escaped character
std::string literal(const std::string& text, size_t quote) {
    std::string s;
    for (size_t i = quote + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char c = text[++i];
            s += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            s += text[i];
        }
    }
    return s;
}

bool word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

} // namespace

int main() {
    int count = 0, missing = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(TOOL_SOURCE_DIR))) {
        if (entry.path().extension() != ".cpp" || entry.path().filename() == "Strings.cpp") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t pos = 0; pos < text.size(); ++pos) {
            size_t quote = std::string::npos;
            if (text.compare(pos, 3, "tr(") == 0 && (pos == 0 || !word(text[pos - 1]))) quote = pos + 3;
            else if ((text.compare(pos, 4, "trf(") == 0 || text.compare(pos, 4, "trn(") == 0) && (pos == 0 || !word(text[pos - 1]))) quote = pos + 4;
            else if (text.compare(pos, 11, "panelTitle(") == 0 && (pos == 0 || !word(text[pos - 1]))) {
                const size_t comma = text.find(',', pos);
                quote = comma == std::string::npos ? comma : comma + 1;
                while (quote < text.size() && text[quote] == ' ') ++quote;
            }
            if (quote == std::string::npos || quote >= text.size() || text[quote] != '"') continue;
            const std::string s = literal(text, quote);
            ++count;
            if (!mkrender::tool::hasTranslation(s.c_str())) {
                std::printf("  FAIL %s: no Korean for \"%s\"\n", entry.path().filename().string().c_str(), s.c_str());
                ++missing;
            }
        }
    }
    if (count < 20) {
        std::printf("FAIL: only %d tr() texts found in %s\n", count, TOOL_SOURCE_DIR);
        return 1;
    }
    mkrender::tool::setLanguage("en");
    if (std::string(mkrender::tool::tr("Save")) != "Save") ++missing;
    if (missing) {
        std::printf("FAIL (%d of %d texts)\n", missing, count);
        return 1;
    }
    std::printf("PASS (%d texts)\n", count);
    return 0;
}
