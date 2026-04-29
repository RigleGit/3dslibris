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
  const u32 *buf = page->GetBuffer();
  const int len = page->GetLength();
  if (!buf || len <= 0)
    return lines;

  std::string line;
  line.reserve((size_t)len);
  int i = 0;
  while (i < len) {
    u32 c = buf[i];
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
        c == TEXT_ITALIC_OFF || c == TEXT_UNDERLINE_ON ||
        c == TEXT_UNDERLINE_OFF || c == TEXT_OVERLINE_ON ||
        c == TEXT_OVERLINE_OFF || c == TEXT_STRIKETHROUGH_ON ||
        c == TEXT_STRIKETHROUGH_OFF || c == TEXT_SUPERSCRIPT_ON ||
        c == TEXT_SUPERSCRIPT_OFF || c == TEXT_SUBSCRIPT_ON ||
        c == TEXT_SUBSCRIPT_OFF || c == TEXT_MONO_ON || c == TEXT_MONO_OFF ||
        c == TEXT_HR || c == TEXT_PRE_ON || c == TEXT_PRE_OFF) {
      i++;
      continue;
    }
    if (c == TEXT_UNDERLINE_STYLE || c == TEXT_FONT_SIZE) {
      if (i + 1 < len)
        i += 2;
      else
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
      if (i + 1 < len)
        i += 2;
      else
        i++;
      continue;
    }
    if (c == TEXT_RTL_LINE_PX) {
      if (i + 1 < len)
        i += 2;  // skip token + data byte
      else
        i++;
      continue;
    }
    if (c == TEXT_IMAGE_ALIGN) {
      if (i + 1 < len)
        i += 2;
      else
        i++;
      continue;
    }
    if (c < 0x80) {
      line.push_back((char)c);
    } else {
      char utf8_buf[4];
      int utf8_len = 0;
      if (c < 0x800) {
        utf8_buf[0] = (char)(0xC0 | (c >> 6));
        utf8_buf[1] = (char)(0x80 | (c & 0x3F));
        utf8_len = 2;
      } else if (c < 0x10000) {
        utf8_buf[0] = (char)(0xE0 | (c >> 12));
        utf8_buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[2] = (char)(0x80 | (c & 0x3F));
        utf8_len = 3;
      } else {
        utf8_buf[0] = (char)(0xF0 | (c >> 18));
        utf8_buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        utf8_buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8_buf[3] = (char)(0x80 | (c & 0x3F));
        utf8_len = 4;
      }
      for (int j = 0; j < utf8_len; j++)
        line.push_back(utf8_buf[j]);
    }
    i++;
  }

  if (!line.empty() || lines.empty())
    lines.push_back(line);
  return lines;
}

} // namespace page_text_extract_utils
