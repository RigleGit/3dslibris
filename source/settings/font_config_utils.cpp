#include "settings/font_config_utils.h"

#include <cstring>

#include "font_constants.h"
#include "shared/text_token_constants.h"

namespace font_config_utils {

namespace {

static const char *kFontTargetLabels[FONT_TARGET_COUNT] = {
    "regular font",     "bold font",       "italic font",    "bold italic font",
    "browser/ui font",  "mono font",       "mono bold font",
    "mono italic font", "mono bold italic font", "fallback font 1",
    "fallback font 2",  "fallback font 3", "fallback font 4"};

static const FontPrefBinding kFontPrefBindings[] = {
    {"normal", TEXT_STYLE_REGULAR},
    {"bold", TEXT_STYLE_BOLD},
    {"italic", TEXT_STYLE_ITALIC},
    {"bolditalic", TEXT_STYLE_BOLDITALIC},
    {"browser", TEXT_STYLE_BROWSER},
    {"mono", TEXT_STYLE_MONO},
    {"monobold", TEXT_STYLE_MONO_BOLD},
    {"monoitalic", TEXT_STYLE_MONO_ITALIC},
    {"monobolditalic", TEXT_STYLE_MONO_BOLDITALIC},
};

} // namespace

const char *GetFontTargetLabel(u8 target) {
  if (target >= FONT_TARGET_COUNT)
    return "";
  return kFontTargetLabels[target];
}

u8 StyleFromTarget(u8 target) {
  switch (target) {
  case FONT_TARGET_BOLD:
    return TEXT_STYLE_BOLD;
  case FONT_TARGET_ITALIC:
    return TEXT_STYLE_ITALIC;
  case FONT_TARGET_BOLDITALIC:
    return TEXT_STYLE_BOLDITALIC;
  case FONT_TARGET_BROWSER:
    return TEXT_STYLE_BROWSER;
  case FONT_TARGET_MONO:
    return TEXT_STYLE_MONO;
  case FONT_TARGET_MONO_BOLD:
    return TEXT_STYLE_MONO_BOLD;
  case FONT_TARGET_MONO_ITALIC:
    return TEXT_STYLE_MONO_ITALIC;
  case FONT_TARGET_MONO_BOLDITALIC:
    return TEXT_STYLE_MONO_BOLDITALIC;
  case FONT_TARGET_REGULAR:
  default:
    return TEXT_STYLE_REGULAR;
  }
}

bool IsFallbackTarget(u8 target) {
  return target >= FONT_TARGET_FALLBACK_1 && target < FONT_TARGET_COUNT;
}

int FallbackIndexFromTarget(u8 target) {
  if (!IsFallbackTarget(target))
    return -1;
  return (int)(target - FONT_TARGET_FALLBACK_1);
}

const char *DefaultFontForStyle(u8 style) {
  switch (style) {
  case TEXT_STYLE_BOLD:
    return FONTBOLDFILE;
  case TEXT_STYLE_ITALIC:
    return FONTITALICFILE;
  case TEXT_STYLE_BOLDITALIC:
    return FONTBOLDITALICFILE;
  case TEXT_STYLE_BROWSER:
    return FONTBROWSERFILE;
  case TEXT_STYLE_MONO:
    return FONTMONOFILE;
  case TEXT_STYLE_MONO_BOLD:
    return FONTMONOBOLDFILE;
  case TEXT_STYLE_MONO_ITALIC:
    return FONTMONOITALICFILE;
  case TEXT_STYLE_MONO_BOLDITALIC:
    return FONTMONOBOLDITALICFILE;
  case TEXT_STYLE_REGULAR:
  default:
    return FONTREGULARFILE;
  }
}

const FontPrefBinding *GetFontPrefBindings() { return kFontPrefBindings; }

size_t GetFontPrefBindingCount() {
  return sizeof(kFontPrefBindings) / sizeof(kFontPrefBindings[0]);
}

bool StyleFromFontPrefAttr(const char *attr_name, u8 *style_out) {
  if (!attr_name || !style_out)
    return false;
  for (size_t i = 0; i < GetFontPrefBindingCount(); i++) {
    if (std::strcmp(kFontPrefBindings[i].attr_name, attr_name) == 0) {
      *style_out = kFontPrefBindings[i].style;
      return true;
    }
  }
  return false;
}

const char *FontPrefAttrForStyle(u8 style) {
  for (size_t i = 0; i < GetFontPrefBindingCount(); i++) {
    if (kFontPrefBindings[i].style == style)
      return kFontPrefBindings[i].attr_name;
  }
  return "";
}

} // namespace font_config_utils
