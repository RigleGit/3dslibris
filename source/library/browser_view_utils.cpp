#include "library/browser_view_utils.h"

#include "ui/theme_colors.h"

namespace browser_view_utils {

namespace {

static unsigned short Rgb565FromU8(float r, float g, float b) {
  if (r < 0.0f)
    r = 0.0f;
  else if (r > 255.0f)
    r = 255.0f;
  if (g < 0.0f)
    g = 0.0f;
  else if (g > 255.0f)
    g = 255.0f;
  if (b < 0.0f)
    b = 0.0f;
  else if (b > 255.0f)
    b = 255.0f;
  return ((unsigned short)(r / 8) << 11) |
         ((unsigned short)(g / 4) << 5) |
         (unsigned short)(b / 8);
}

} // namespace

int PageSize(BrowserViewMode mode) {
  return mode == BROWSER_VIEW_LIST ? 7 : 4;
}

int ColumnCount(BrowserViewMode mode) {
  return mode == BROWSER_VIEW_LIST ? 1 : 2;
}

int ListTitleMaxLines() { return 2; }

int ListTitleBoxHeight(int line_height) {
  if (line_height < 1)
    line_height = 1;
  return ListTitleMaxLines() * line_height + 12;
}

bool ShouldLoadCovers(BrowserViewMode mode) {
  return mode != BROWSER_VIEW_LIST;
}

const char *Label(BrowserViewMode mode) {
  return mode == BROWSER_VIEW_LIST ? "List" : "Gallery";
}

BrowserViewMode ParsePrefValue(const char *value) {
  if (value && value[0] == 'l' && value[1] == 'i' && value[2] == 's' &&
      value[3] == 't' && value[4] == '\0') {
    return BROWSER_VIEW_LIST;
  }
  return BROWSER_VIEW_GALLERY;
}

const char *ToPrefValue(BrowserViewMode mode) {
  return mode == BROWSER_VIEW_LIST ? "list" : "gallery";
}

ListRowPalette PaletteForListRow(bool selected, int colorMode) {
  ListRowPalette palette;
  const ThemePalette &theme = GetThemePalette(colorMode);
  if (selected) {
    palette.fill = Rgb565FromU8(theme.btnFillTopR, theme.btnFillTopG, theme.btnFillTopB);
    palette.border = Rgb565FromU8(theme.btnBorderOuterR, theme.btnBorderOuterG, theme.btnBorderOuterB);
    palette.text = theme.textFgColor;
    return palette;
  }
  palette.fill = Rgb565FromU8(theme.btnFillBotR, theme.btnFillBotG, theme.btnFillBotB);
  palette.border = Rgb565FromU8(theme.btnBorderInnerR, theme.btnBorderInnerG, theme.btnBorderInnerB);
  palette.text = theme.textFgColor;
  return palette;
}

} // namespace browser_view_utils
