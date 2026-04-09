#pragma once

#include <cstddef>
#include <3ds.h>

namespace font_config_utils {

enum FontTarget : u8 {
  FONT_TARGET_REGULAR = 0,
  FONT_TARGET_BOLD = 1,
  FONT_TARGET_ITALIC = 2,
  FONT_TARGET_BOLDITALIC = 3,
  FONT_TARGET_BROWSER = 4,
  FONT_TARGET_MONO = 5,
  FONT_TARGET_MONO_BOLD = 6,
  FONT_TARGET_MONO_ITALIC = 7,
  FONT_TARGET_MONO_BOLDITALIC = 8,
  FONT_TARGET_FALLBACK_1 = 9,
  FONT_TARGET_FALLBACK_2 = 10,
  FONT_TARGET_FALLBACK_3 = 11,
  FONT_TARGET_FALLBACK_4 = 12,
  FONT_TARGET_COUNT = 13
};

struct FontPrefBinding {
  const char *attr_name;
  u8 style;
};

const char *GetFontTargetLabel(u8 target);
u8 StyleFromTarget(u8 target);
bool IsFallbackTarget(u8 target);
int FallbackIndexFromTarget(u8 target);
const char *DefaultFontForStyle(u8 style);

const FontPrefBinding *GetFontPrefBindings();
size_t GetFontPrefBindingCount();
bool StyleFromFontPrefAttr(const char *attr_name, u8 *style_out);
const char *FontPrefAttrForStyle(u8 style);

} // namespace font_config_utils
