#include "book/page_alignment_utils.h"

#include "shared/text_token_constants.h"

namespace page_alignment_utils {

namespace {

enum TextStyleId {
  kTextStyleRegular = 0,
  kTextStyleBold = 1,
  kTextStyleItalic = 2,
  kTextStyleBoldItalic = 3,
  kTextStyleMono = 5,
  kTextStyleMonoBold = 6,
  kTextStyleMonoItalic = 7,
  kTextStyleMonoBoldItalic = 8,
};

unsigned char ResolveTextStyle(bool bold, bool italic, bool mono) {
  if (mono && bold && italic)
    return kTextStyleMonoBoldItalic;
  if (mono && bold)
    return kTextStyleMonoBold;
  if (mono && italic)
    return kTextStyleMonoItalic;
  if (mono)
    return kTextStyleMono;
  if (bold && italic)
    return kTextStyleBoldItalic;
  if (bold)
    return kTextStyleBold;
  if (italic)
    return kTextStyleItalic;
  return kTextStyleRegular;
}

} // namespace

int MeasureAlignedLineWidth(const uint32_t *buf, size_t length, size_t start,
                            bool bold, bool italic, bool mono,
                            MeasureGlyphFn measure, void *ctx) {
  if (!buf || !measure || start >= length)
    return 0;

  int line_width = 0;
  bool scan_bold = bold;
  bool scan_italic = italic;
  bool scan_mono = mono;

  for (size_t scan = start; scan < length; ++scan) {
    const uint32_t sc = buf[scan];
    if (sc == '\n')
      break;

    switch (sc) {
    case TEXT_BOLD_ON:
      scan_bold = true;
      continue;
    case TEXT_BOLD_OFF:
      scan_bold = false;
      continue;
    case TEXT_ITALIC_ON:
      scan_italic = true;
      continue;
    case TEXT_ITALIC_OFF:
      scan_italic = false;
      continue;
    case TEXT_MONO_ON:
      scan_mono = true;
      continue;
    case TEXT_MONO_OFF:
      scan_mono = false;
      continue;
    case TEXT_UNDERLINE_ON:
    case TEXT_UNDERLINE_OFF:
    case TEXT_STRIKETHROUGH_ON:
    case TEXT_STRIKETHROUGH_OFF:
    case TEXT_SUPERSCRIPT_ON:
    case TEXT_SUPERSCRIPT_OFF:
    case TEXT_SUBSCRIPT_ON:
    case TEXT_SUBSCRIPT_OFF:
    case TEXT_OVERLINE_ON:
    case TEXT_OVERLINE_OFF:
    case TEXT_PRE_ON:
    case TEXT_PRE_OFF:
    case TEXT_PARAGRAPH_LEFT:
    case TEXT_PARAGRAPH_CENTER:
    case TEXT_PARAGRAPH_RIGHT:
    case TEXT_PARAGRAPH_LTR:
    case TEXT_PARAGRAPH_RTL:
    case TEXT_IMAGE_LEADING_PARAGRAPH:
    case TEXT_IMAGE_FIGURE_WITH_CAPTION:
    case TEXT_IMAGE_CONTEXT_DEFAULT:
      continue;
    case TEXT_IMAGE:
    case TEXT_HR:
      return line_width;
    case TEXT_UNDERLINE_STYLE:
    case TEXT_FONT_SIZE:
    case TEXT_RTL_LINE_PX:
    case TEXT_LINE_PX:
    case TEXT_LINK_START:
      ++scan;
      continue;
    case TEXT_LINK_END:
      continue;
    default:
      if (sc < 32)
        return line_width;
      line_width +=
          measure(sc, ResolveTextStyle(scan_bold, scan_italic, scan_mono), ctx);
      break;
    }
  }

  return line_width;
}

} // namespace page_alignment_utils
