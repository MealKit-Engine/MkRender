#include "mkcontract/Modules.h"

#include <cstdlib>
#include <utility>

namespace mkcontract {

ModuleList parseModuleList(const std::string& output) {
    ModuleList list;
    size_t start = 0;
    while (start < output.size()) {
        size_t nl = output.find('\n', start);
        if (nl == std::string::npos) nl = output.size();
        std::string line = output.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', from)) {
            f.push_back(line.substr(from, tab - from));
            from = tab + 1;
        }
        f.push_back(line.substr(from));
        const auto at = [&f](size_t i) { return i < f.size() ? f[i] : std::string(); }; // later columns are optional
        if (f[0] == "installed" && f.size() >= 6) {
            InstalledModule m{f[1], f[2], f[5], std::atoi(f[3].c_str()), std::atoi(f[4].c_str())};
            m.type = at(6);
            m.title = at(7);
            m.summary = at(8);
            m.tags = at(9);
            m.icon = at(10);
            list.installed.push_back(std::move(m));
        } else if (f[0] == "module" && f.size() >= 10) {
            RegistryModule r{f[1], f[2] == "-" ? std::string() : f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]};
            r.icon = at(10);
            list.registry.push_back(std::move(r));
        } else if (f[0] == "error" && f.size() >= 2) {
            list.error = f[1];
        }
    }
    return list;
}

const char* const kModuleIconKeywords[] = {"gameplay", "game", "timer", "combat", "health", "ai", "map", "save", "ui",
                                           "audio", "render", "rhi", "camera", "physics", "net", "online", "input", "data",
                                           "asset", "scene", "debug", "tool", "editor", "settings", "loc", "feedback", "live",
                                           "session", "sim"};
const int kModuleIconKeywordCount = static_cast<int>(sizeof(kModuleIconKeywords) / sizeof(kModuleIconKeywords[0]));

namespace {
bool isIconKeyword(const std::string& word) {
    for (int i = 0; i < kModuleIconKeywordCount; ++i)
        if (word == kModuleIconKeywords[i]) return true;
    return false;
}
} // namespace

std::string moduleIconKeyword(const std::string& icon, const std::string& tags, const std::string& name) {
    if (isIconKeyword(icon)) return icon;
    size_t start = 0;
    while (start < tags.size()) {
        size_t end = tags.find(' ', start);
        if (end == std::string::npos) end = tags.size();
        const std::string tag = tags.substr(start, end - start);
        if (tag != "engine" && isIconKeyword(tag)) return tag;
        start = end + 1;
    }
    if (isIconKeyword(name)) return name;
    const size_t dash = name.find('-');
    if (dash != std::string::npos && isIconKeyword(name.substr(0, dash))) return name.substr(0, dash);
    return std::string();
}

bool hasTag(const std::string& tags, const std::string& tag) {
    if (tag.empty()) return false;
    for (size_t at = tags.find(tag); at != std::string::npos; at = tags.find(tag, at + 1)) {
        const size_t end = at + tag.size();
        if ((at == 0 || tags[at - 1] == ' ') && (end == tags.size() || tags[end] == ' ')) return true;
    }
    return false;
}

} // namespace mkcontract
