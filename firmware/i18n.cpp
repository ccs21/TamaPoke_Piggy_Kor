#include "i18n.h"

#include "dex.h"
#include "generated/dex_names_ko.h"
#include "generated/help_ko.h"
#include "generated/i18n_strings_ko.h"
#include "pet.h"

Lang gLang = LANG_DEFAULT;

const char *T(StrId id) {
  if (id >= STR_COUNT) return "?";
  return STRINGS_KO[id];
}

const char *dexName(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return "?";
  return DEX_NAMES_KO[dex];
}

const char *medalName(int i) {
  if (i < 0 || i >= MED_COUNT) return "?";
  return MED_NAME_KO[i];
}

const char *medalLabel(int i) {
  if (i < 0 || i >= MED_COUNT) return "?";
  return MED_LBL_KO[i];
}

const char *medalDesc(int i) {
  if (i < 0 || i >= MED_COUNT) return "?";
  return MED_DSC_KO[i];
}

const char *typeNameKo(uint8_t type) {
  if (type >= 19) return "-";
  return TYPE_NAMES_KO[type];
}

const char *helpWordKo() { return HELP_WORD_KO; }

const char *helpOkKo() { return HELP_OK_KO; }

const char *helpTitleKo(uint8_t page) {
  if (page >= 8) return "?";
  return HELP_TITLES_KO[page];
}

const char *helpLineKo(uint8_t page, uint8_t line) {
  if (page >= 8 || line >= 6) return "?";
  return HELP_LINES_KO[page][line];
}

void loadLang() { gLang = LANG_DEFAULT; }

void setLang(Lang) { gLang = LANG_DEFAULT; }
