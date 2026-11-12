#pragma once

#include <string>
#include <vector>

// Source module list (docs/CONTRACT.md "Module list"): what the engine's `tools\module.ps1 search -Tsv -Project <folder>` prints,
// one tab-separated line per item. The editor [Modules] window and MealKit Manager read it. Unknown lines are skipped.
//   installed <name> <version> <modified> <pending> <source> [<type> <title> <summary> <tags> <icon>]
//   module    <name> <version or -> <latest> <latestEngine> <title> <summary> <tags> <author> <repo> [<icon>]
// Bracketed columns came later (contract 0.2): an older engine leaves them out and they read as blank.
//   error     <why the registry list couldn't be fetched>
namespace mkcontract {

struct InstalledModule {
    std::string name, version, source;
    int modified = 0; // number of files the game modified after install
    int pending = 0;  // number of .new files left by an update
    std::string type;                 // "engine" or "source" (blank from an older engine)
    std::string title, summary, tags; // from the module's module.txt; tags are space-separated
    std::string icon;                 // module.txt "icon" keyword (blank: none given)
};

struct RegistryModule {
    std::string name;
    std::string version;      // newest version for the project's engine MINOR (blank if none)
    std::string latest;       // newest version
    std::string latestEngine; // engine MAJOR.MINOR of that version
    std::string title, summary, tags, author, repo;
    std::string icon; // module.txt "icon" keyword (blank: none given)
};

struct ModuleList {
    std::vector<InstalledModule> installed;
    std::vector<RegistryModule> registry;
    std::string error;
};

ModuleList parseModuleList(const std::string& output);

// Whether a space-separated tag list has this tag (exact word match)
bool hasTag(const std::string& tags, const std::string& tag);

// Icon keywords (docs/CONTRACT.md "Module icons"): every program draws its own picture for each, in this order
extern const char* const kModuleIconKeywords[];
extern const int kModuleIconKeywordCount;

// The icon keyword a module shows: its "icon" if that is a keyword, else the first tag that is one, else its name
// (engine module names are keywords; a backend like online-steam takes online's), else "" (each program draws a default)
std::string moduleIconKeyword(const std::string& icon, const std::string& tags, const std::string& name);

} // namespace mkcontract
