#pragma once
#include <pebble.h>

// The watch shows German when its system language is German, English otherwise.
extern bool g_lang_de;

// L("Rest", "Pause"): picks the text for the watch's language.
#define L(en, de) (g_lang_de ? (de) : (en))

void i18n_init(void);

// Two-letter language code sent to the phone so its messages match ("de" or "en").
const char *i18n_lang(void);
