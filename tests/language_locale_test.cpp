// tests for runtime/src/language_locale.h: operating-system locale -> the game's language.
#include <cstdio>
#include <string>

#include "language_locale.h"

int main() {
    struct Case { const char* locale; const char* want; };
    const Case cases[] = {
        {"ko-KR", "ko"}, {"ko_KR.UTF-8", "ko"}, {"ja-JP", "ja"}, {"de_DE@euro", "de"}, {"it", "it"},
        {"zh-CN", "zh-hans"}, {"zh-Hans-CN", "zh-hans"}, {"zh_SG", "zh-hans"}, {"zh", "zh-hans"},
        {"zh-TW", "zh-hant"}, {"zh-Hant-TW", "zh-hant"}, {"zh_HK.UTF-8", "zh-hant"}, {"zh-Hant", "zh-hant"},
        {"en-US", "en-us"}, {"en", "en-us"}, {"en_GB.UTF-8", "en-gb"}, {"en-AU", "en-gb"}, {"en-CA", "en-us"},
        {"fr-FR", "fr"}, {"fr-CA", "fr-ca"}, {"pt-PT", "pt"}, {"pt-BR", "pt-br"},
        {"es-ES", "es"}, {"es", "es"}, {"es-MX", "es-419"}, {"es_AR.UTF-8", "es-419"}, {"es-419", "es-419"},
        {"nl-NL", "nl"}, {"ru-RU", "ru"},
        {"pl-PL", ""}, {"", ""}, {"C", ""}, {"tr_TR.UTF-8", ""},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const std::string got = nx::language_for_locale(c.locale);
        if (got != c.want) {
            std::printf("FAIL: %s -> \"%s\", want \"%s\"\n", c.locale, got.c_str(), c.want);
            failures++;
        }
    }
    if (failures) return 1;
    std::printf("language_locale: ok\n");
    return 0;
}
