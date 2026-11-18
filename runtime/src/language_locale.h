// SPDX-License-Identifier: GPL-3.0-or-later
// The game language that matches an operating-system locale, for `language=system` (and an unset language): the
// game follows the language the player's PC or phone is set to, as the console follows its system language.
//
// Pure, no platform calls: tests/language_locale_test.cpp drives it. language.cpp reads the locale itself.
#pragma once

#include <cctype>
#include <string>

namespace nx {

// "ko_KR.UTF-8", "zh-Hant-TW", "pt-BR", "en-GB" ... -> a name desired_language() accepts ("ko", "zh-hant", "pt-br",
// "en-gb"); "" for a locale the game has no text for (the caller then uses en-US).
inline std::string language_for_locale(std::string locale) {
    const size_t cut = locale.find_first_of(".@");
    if (cut != std::string::npos) locale.resize(cut);
    for (char& c : locale) c = c == '_' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
    while (!locale.empty() && (locale.back() == '-' || std::isspace(static_cast<unsigned char>(locale.back()))))
        locale.pop_back();
    const std::string lang = locale.substr(0, locale.find('-'));
    const std::string rest = locale.size() > lang.size() ? locale.substr(lang.size()) + "-" : std::string();
    auto has = [&](const char* part) { return rest.find(std::string("-") + part + "-") != std::string::npos; };
    if (lang == "zh") {
        if (has("hant") || has("tw") || has("hk") || has("mo")) return "zh-hant";
        return "zh-hans";
    }
    if (lang == "en") return has("gb") || has("ie") || has("au") || has("nz") ? "en-gb" : "en-us";
    if (lang == "fr") return has("ca") ? "fr-ca" : "fr";
    if (lang == "pt") return has("br") ? "pt-br" : "pt";
    if (lang == "es") return rest.empty() || has("es") ? "es" : "es-419";  // Spain's Spanish, else Latin American
    for (const char* known : {"ja", "de", "it", "nl", "ru", "ko"})
        if (lang == known) return lang;
    return "";
}

}  // namespace nx
