#include "i18n.h"

bool g_lang_de;

void i18n_init(void) {
#ifdef FORCE_LANG_DE
  g_lang_de = FORCE_LANG_DE;
  return;
#endif
  const char *loc = i18n_get_system_locale();
  g_lang_de = loc && strncmp(loc, "de", 2) == 0;
}

const char *i18n_lang(void) {
  return g_lang_de ? "de" : "en";
}
