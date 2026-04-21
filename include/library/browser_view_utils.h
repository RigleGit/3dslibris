#pragma once

#include "library/browser_view_mode.h"

namespace browser_view_utils {

struct ListRowPalette {
  unsigned short fill;
  unsigned short border;
  unsigned short text;
};

int PageSize(BrowserViewMode mode);
int ColumnCount(BrowserViewMode mode);
int ListTitleMaxLines();
int ListTitleBoxHeight(int line_height);
bool ShouldLoadCovers(BrowserViewMode mode);
const char *Label(BrowserViewMode mode);
BrowserViewMode ParsePrefValue(const char *value);
const char *ToPrefValue(BrowserViewMode mode);
ListRowPalette PaletteForListRow(bool selected, int colorMode);

} // namespace browser_view_utils
