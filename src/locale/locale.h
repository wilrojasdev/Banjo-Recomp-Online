#ifndef __BANJO_LOCALE_H__
#define __BANJO_LOCALE_H__

#include <string>
#include "banjo_config.h"

namespace banjo::locale {
    // Read the language from the config and cache it. Call once after config load.
    void init();

    // Re-read the language from the config. Returns true if it changed.
    bool refresh();

    // Language the process was launched with (never changes during the session).
    Language get_startup_language();

    // Current language as of the last refresh().
    Language get_current();

    // Translate a key to the current language. Falls back to English, then to the key itself.
    const std::string& tr(const char* key);

    // Persist `lang` to language.txt right now, regardless of the recompui::config
    // commit/apply lifecycle. Use this when the user makes a choice that should
    // survive a restart even if they didn't formally Apply (e.g. clicking a
    // language radio button in the options menu).
    void commit_language_to_disk(Language lang);
}

#endif
