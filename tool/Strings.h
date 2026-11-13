#pragma once

// Screen text and the suite language (engine docs/TOOL_DESIGN.md section 4 "Text" and "Language"). English is in the code, the
// other languages in the table in Strings.cpp. The language is the suite's own: the "language" line of editor_settings.txt in the
// MealKit user folder (MealKit-Contract "User folder"), which MealKit Manager and every Mk tool read and write, so changing it
// anywhere changes it everywhere, also in tools that are open (followLanguage).
#include <string>

namespace mkrender::tool {

struct Language {
    const char* code; // editor_settings.txt value
    const char* name; // in its own language, so anyone finds theirs in the list whatever the current language is
};
inline constexpr Language kLanguages[] = {{"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"}, {"en", "English"}}; // 한국어

void setLanguage(const std::string& code); // anything but "en" is Korean, like MealKit Manager
const char* language();                    // "ko" or "en"
const char* tr(const char* english);       // the text in the current language (the English itself when it has no line)
// Marks text in a static table for the Korean table and its test; the code calls tr() on it where it is shown
constexpr const char* trn(const char* english) { return english; }
std::string trf(const char* english, ...); // tr, then filled in as a printf format
bool hasTranslation(const char* english);  // tests: whether the Korean table has this line

void loadLanguage();                       // from editor_settings.txt; Korean when the line is missing (the Manager's default)
bool saveLanguage(const std::string& code); // to editor_settings.txt, other settings kept; false when the folder can't be written
bool followLanguage();                     // call once a frame: re-reads the file once a second when it changed; true when the language changed

} // namespace mkrender::tool
