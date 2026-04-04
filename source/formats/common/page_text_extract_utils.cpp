/*
    3dslibris - page_text_extract_utils.cpp

    Helpers to extract plain text lines from parsed pages.
*/

#include "formats/common/page_text_extract_utils.h"

#include "book/page.h"
#include "formats/mobi/mobi_heading_markers.h"
#include "shared/text_token_constants.h"

namespace page_text_extract_utils {

std::vector<std::string> ExtractTextLinesFromPage(Page *page) {
  std::vector<std::string> lines;
  if (!page)
    return lines;
  const u8 *buf = page->GetBuffer();
  const int len = page->GetLength();
  if (!buf || len <= 0)
    return lines;

  std::string line;
  line.reserve((size_t)len);
  int i = 0;
  while (i < len) {
    u8 c = buf[i];
    if (c == '\r' || c == '\n') {
      lines.push_back(line);
      line.clear();
      i++;
      continue;
    }
    if (c == TEXT_IMAGE_CONTEXT_DEFAULT ||
        c == TEXT_IMAGE_LEADING_PARAGRAPH ||
        c == TEXT_IMAGE_FIGURE_WITH_CAPTION) {
      i++;
      continue;
    }
    if (c == TEXT_BOLD_ON || c == TEXT_BOLD_OFF || c == TEXT_ITALIC_ON ||
        c == TEXT_ITALIC_OFF) {
      i++;
      continue;
    }
    if (mobi_heading_markers::HeadingLevelFromMarker(c) > 0) {
      i++;
      continue;
    }
    if (c == TEXT_IMAGE) {
      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
      }
      lines.push_back(std::string());
      if (i + 2 < len)
        i += 3;
      else
        i++;
      continue;
    }
    line.push_back((char)c);
    i++;
  }

  if (!line.empty() || lines.empty())
    lines.push_back(line);
  return lines;
}

} // namespace page_text_extract_utils
