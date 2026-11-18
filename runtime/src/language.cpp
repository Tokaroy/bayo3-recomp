// SPDX-License-Identifier: GPL-3.0-or-later
// The operating system's language, for language=system (language.h, language_locale.h).
#include "language.h"

#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace nx {

std::string system_locale() {
#if defined(_WIN32)
    // The display language the player chose (Settings > Time & language), not the regional format.
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        std::string out;
        for (const wchar_t* p = name; *p; p++) out.push_back(*p < 128 ? char(*p) : '?');
        return out;
    }
    return "";
#elif defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    for (const char* key : {"persist.sys.locale", "ro.product.locale"})
        if (__system_property_get(key, value) > 0) return value;
    return "";
#else
    for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(key);
        if (v && *v && std::string(v) != "C" && std::string(v) != "POSIX") return v;
    }
    return "";
#endif
}

}  // namespace nx
