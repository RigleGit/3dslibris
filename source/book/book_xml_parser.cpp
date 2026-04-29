/*
    3dslibris - book.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Core book state/container logic shared across EPUB/FB2/TXT/RTF/ODT.
    - Chapter/bookmark management and TOC target resolution helpers.
    - Page ownership/lifetime and parser integration points.
*/

#include "book/book.h"

#include "book/book_xml_block_utils.h"
#include "book/book_context.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_hidden_utils.h"
#include "book/book_xml_list_utils.h"
#include "book/epub_css_class_map.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_table_utils.h"
#include "book/book_xml_text_emit.h"
#include "book/book_xml.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/html_entity_utils.h"
#include "reader/inline_link_utils.h"
#include "book/heading_layout.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "debug_log.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_render_layout_utils.h"
#include "shared/text_bidi_utils.h"
#include "shared/text_unicode_utils.h"
#include "shared/string_utils.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

namespace {

static const size_t kFb2BinaryMaxChars = 6 * 1024 * 1024;

static bool EqualsAsciiNoCase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static std::string ToLowerAsciiLocal(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static void AppendParsedByte(parsedata_t *p, u32 c) {
  parse_append_page_byte(p, c);
}

static void RestoreParsedInlineLinkMarker(parsedata_t *p);

struct ParsedTextMeasureContext {
  Text *text;
  u8 style;
};

static void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}

static ParsedTextMeasureContext MakeParsedTextMeasureContext(Text *text,
                                                             bool bold,
                                                             bool italic,
                                                             bool mono) {
  ParsedTextMeasureContext ctx{};
  ctx.text = text;
  ctx.style =
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono);
  return ctx;
}

static int MeasureParsedTextAdvance(uint32_t codepoint, void *ctx) {
  ParsedTextMeasureContext *measure = (ParsedTextMeasureContext *)ctx;
  if (!measure || !measure->text)
    return 0;
  return measure->text->GetAdvance(codepoint, measure->style);
}

static bool HasVisibleTextContentUtf8(const char *txt, int txtlen) {
  if (!txt || txtlen <= 0)
    return false;
  size_t offset = 0;
  while (offset < (size_t)txtlen) {
    uint32_t cp = 0;
    const size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        txt + offset, (size_t)txtlen - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    if (!iswhitespace((u32)cp))
      return true;
    offset += consumed;
  }
  return false;
}

static bool ParseInAnyEasyParagraphTightBlock(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (book_xml_block_utils::SuppressInnerParagraphSpacing(p->stack[i]))
      return true;
  }
  return false;
}

static void ApplyDeferredStyleSync(parsedata_t *p, Text *ts) {
  if (!p || !p->deferred_style_sync)
    return;

  bool style_changed = false;
  if (p->bold != p->deferred_target_bold) {
    AppendParsedByte(p, p->deferred_target_bold ? TEXT_BOLD_ON : TEXT_BOLD_OFF);
    if (p->deferred_target_bold)
      p->pos++;
    p->bold = p->deferred_target_bold;
    style_changed = true;
  }
  if (p->italic != p->deferred_target_italic) {
    AppendParsedByte(p, p->deferred_target_italic ? TEXT_ITALIC_ON
                                                  : TEXT_ITALIC_OFF);
    p->italic = p->deferred_target_italic;
    style_changed = true;
  }
  if (p->underline != p->deferred_target_underline) {
    AppendParsedByte(p, p->deferred_target_underline ? TEXT_UNDERLINE_ON
                                                     : TEXT_UNDERLINE_OFF);
    p->underline = p->deferred_target_underline;
    if (!p->underline)
      p->underline_style = UNDERLINE_STYLE_SOLID;
    style_changed = true;
  }
  if (p->underline &&
      p->underline_style != p->deferred_target_underline_style) {
    p->underline_style = p->deferred_target_underline_style;
    book_xml_parser_style_utils::EmitUnderlineStyleMarker(
        p, p->underline_style);
  }
  if (p->overline != p->deferred_target_overline) {
    AppendParsedByte(p, p->deferred_target_overline ? TEXT_OVERLINE_ON
                                                    : TEXT_OVERLINE_OFF);
    p->overline = p->deferred_target_overline;
    style_changed = true;
  }
  if (p->strikethrough != p->deferred_target_strikethrough) {
    AppendParsedByte(p, p->deferred_target_strikethrough ? TEXT_STRIKETHROUGH_ON
                                                         : TEXT_STRIKETHROUGH_OFF);
    p->strikethrough = p->deferred_target_strikethrough;
    style_changed = true;
  }
  if (p->superscript != p->deferred_target_superscript) {
    AppendParsedByte(p, p->deferred_target_superscript ? TEXT_SUPERSCRIPT_ON
                                                       : TEXT_SUPERSCRIPT_OFF);
    p->superscript = p->deferred_target_superscript;
    style_changed = true;
  }
  if (p->subscript != p->deferred_target_subscript) {
    AppendParsedByte(p, p->deferred_target_subscript ? TEXT_SUBSCRIPT_ON
                                                     : TEXT_SUBSCRIPT_OFF);
    p->subscript = p->deferred_target_subscript;
    style_changed = true;
  }
  if (p->mono != p->deferred_target_mono) {
    AppendParsedByte(p, p->deferred_target_mono ? TEXT_MONO_ON : TEXT_MONO_OFF);
    p->mono = p->deferred_target_mono;
    style_changed = true;
  }

  p->deferred_style_sync = false;
  if (style_changed)
    SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
}

static void QueueDeferredStyleSync(parsedata_t *p, bool want_bold,
                                   bool want_italic, bool want_underline,
                                   u8 want_underline_style,
                                   bool want_overline,
                                   bool want_strikethrough,
                                   bool want_superscript, bool want_subscript,
                                   bool want_mono) {
  if (!p)
    return;
  p->deferred_style_sync = true;
  p->deferred_target_bold = want_bold;
  p->deferred_target_italic = want_italic;
  p->deferred_target_underline = want_underline;
  p->deferred_target_underline_style = want_underline_style;
  p->deferred_target_overline = want_overline;
  p->deferred_target_strikethrough = want_strikethrough;
  p->deferred_target_superscript = want_superscript;
  p->deferred_target_subscript = want_subscript;
  p->deferred_target_mono = want_mono;
}

static bool ContainsAsciiNoCase(const std::string &haystack,
                                const char *needle) {
  if (!needle || !needle[0])
    return false;
  const std::string haystack_lc = ToLowerAsciiLocal(haystack);
  const std::string needle_lc = ToLowerAsciiLocal(needle);
  return haystack_lc.find(needle_lc) != std::string::npos;
}

static bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static book_xml_css_style_utils::MarginTopResult
ParseMarginTopPx(const char *style) {
  return book_xml_css_style_utils::ParseMarginTop(style);
}

// Parse a CSS/HTML width value and return pixels (>0), or 0 if unconstrained.
// Handles: %, px, bare integer (HTML width attr), em/rem, pt, cm, mm, in, vw.
// text_width = available text area in pixels; font_px = current font height in px.
// screen_width is fixed at 240 on 3DS.
static int ParseCssLengthPx(const char *v, int text_width, int font_px) {
  if (!v || !*v)
    return 0;
  while (*v == ' ')
    v++;
  if (!*v)
    return 0;

  // Numeric part: integer * 1000 + fractional (3 decimal places of precision)
  int num1000 = 0; // value * 1000
  bool has_digit = false;
  while (*v >= '0' && *v <= '9') {
    num1000 = num1000 * 10 + (*v - '0') * 1000;
    has_digit = true;
    v++;
    if (num1000 > 100000 * 1000) // overflow guard (>100000 px is nonsense)
      return 0;
  }
  if (*v == '.') {
    v++;
    int place = 100;
    while (*v >= '0' && *v <= '9' && place > 0) {
      num1000 += (*v - '0') * place;
      place /= 10;
      v++;
      has_digit = true;
    }
    while (*v >= '0' && *v <= '9')
      v++; // consume remaining decimal digits
  }
  if (!has_digit)
    return 0;
  while (*v == ' ')
    v++;

  int result_px = 0;
  if (*v == '%') {
    // Percentage of text width. Cap at 100% (10000 hundredths).
    if (num1000 <= 0 || num1000 > 100 * 1000)
      return 0;
    result_px = (text_width * num1000 + 50000) / 100000;
  } else if (*v == '\0') {
    // Bare integer HTML attribute (e.g. width="50") — treated as px.
    result_px = (num1000 + 500) / 1000;
  } else if (v[0] == 'p' && v[1] == 'x') {
    result_px = (num1000 + 500) / 1000;
  } else if ((v[0] == 'e' && v[1] == 'm') ||
             (v[0] == 'r' && v[1] == 'e' && v[2] == 'm')) {
    // em/rem relative to current font size.
    result_px = (num1000 * font_px + 500) / 1000;
  } else if (v[0] == 'p' && v[1] == 't') {
    // 1pt = 4/3 px at 96 DPI.
    result_px = (num1000 * 4 + 1500) / 3000;
  } else if (v[0] == 'c' && v[1] == 'm') {
    // 1cm = 37.795px at 96 DPI; use 378/10.
    result_px = (num1000 * 378 + 5000) / 10000;
  } else if (v[0] == 'm' && v[1] == 'm') {
    // 1mm = 3.7795px; use 378/100.
    result_px = (num1000 * 378 + 50000) / 100000;
  } else if (v[0] == 'i' && v[1] == 'n') {
    // 1in = 96px.
    result_px = (num1000 * 96 + 500) / 1000;
  } else if (v[0] == 'v' && v[1] == 'w') {
    // vw: percentage of 240px viewport width.
    if (num1000 <= 0 || num1000 > 100 * 1000)
      return 0;
    result_px = (240 * num1000 + 50000) / 100000;
  } else {
    // Unknown unit (ch, ex, svw, dvw, max-content, etc.) — don't constrain.
    return 0;
  }

  return std::max(0, std::min(result_px, text_width));
}

// Return the author-specified max width in pixels for an img element.
// Checks the HTML width attribute first, then the CSS width property in style.
// Returns 0 if no usable constraint found.
static int ParseImgWidthPx(const char *width_attr, const char *style,
                            int text_width, int font_px) {
  if (width_attr && *width_attr) {
    int px = ParseCssLengthPx(width_attr, text_width, font_px);
    if (px > 0)
      return px;
  }
  if (style && *style) {
    std::string lc = ToLowerAscii(std::string(style));
    size_t pos = lc.find("width:");
    if (pos != std::string::npos) {
      pos += 6;
      while (pos < lc.size() && lc[pos] == ' ')
        pos++;
      int px = ParseCssLengthPx(lc.c_str() + pos, text_width, font_px);
      if (px > 0)
        return px;
    }
  }
  return 0;
}

static void ParseClassStyleFlags(const char *class_name, bool *bold_out,
                                 bool *italic_out, bool *underline_out,
                                 u8 *underline_style_out,
                                 bool *overline_out, bool *strikethrough_out,
                                 bool *superscript_out,
                                 bool *subscript_out) {
  if (!class_name ||
      (!bold_out && !italic_out && !underline_out && !overline_out &&
       !strikethrough_out &&
       !superscript_out && !subscript_out))
    return;
  const std::string class_lc = ToLowerAsciiLocal(class_name);

  if (italic_out) {
    if (ContainsAsciiNoCase(class_lc, "italic") ||
        ContainsAsciiNoCase(class_lc, "oblique") ||
        ContainsAsciiNoCase(class_lc, "emphasis")) {
      *italic_out = true;
    }
  }

  if (bold_out) {
    if (ContainsAsciiNoCase(class_lc, "bold") ||
        ContainsAsciiNoCase(class_lc, "semibold") ||
        ContainsAsciiNoCase(class_lc, "font-weight")) {
      *bold_out = true;
    }
  }

  if (underline_out) {
    if (ContainsAsciiNoCase(class_lc, "underline") ||
        ContainsAsciiNoCase(class_lc, "underlined")) {
      *underline_out = true;
      if (underline_style_out) {
        if (ContainsAsciiNoCase(class_lc, "wavy"))
          *underline_style_out = UNDERLINE_STYLE_WAVY;
        else if (ContainsAsciiNoCase(class_lc, "dashed"))
          *underline_style_out = UNDERLINE_STYLE_DASHED;
        else if (ContainsAsciiNoCase(class_lc, "dotted"))
          *underline_style_out = UNDERLINE_STYLE_DOTTED;
      }
    }
  }

  if (overline_out) {
    if (ContainsAsciiNoCase(class_lc, "overline") ||
        ContainsAsciiNoCase(class_lc, "overlined")) {
      *overline_out = true;
    }
  }

  if (strikethrough_out) {
    if (ContainsAsciiNoCase(class_lc, "strikethrough") ||
        ContainsAsciiNoCase(class_lc, "line-through") ||
        ContainsAsciiNoCase(class_lc, "strike") ||
        ContainsAsciiNoCase(class_lc, "deleted")) {
      *strikethrough_out = true;
    }
  }

  if (superscript_out) {
    if (ContainsAsciiNoCase(class_lc, "superscript")) {
      *superscript_out = true;
    }
  }

  if (subscript_out) {
    if (ContainsAsciiNoCase(class_lc, "subscript")) {
      *subscript_out = true;
    }
  }
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopPx(const char **attr) {
  book_xml_css_style_utils::MarginTopResult empty;
  if (!attr)
    return empty;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "style"))
      return ParseMarginTopPx(attr[i + 1]);
  }
  return empty;
}

static std::string ExtractStyleAttr(const char **attr) {
  if (!attr)
    return {};
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "style"))
      return std::string(attr[i + 1]);
  }
  return {};
}

static std::string ExtractClassAttr(const char **attr) {
  if (!attr)
    return {};
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "class"))
      return std::string(attr[i + 1]);
  }
  return {};
}

static book_xml_css_style_utils::MarginTopResult
LookupClassMarginTop(const std::string &class_attr,
                     const epub_css_class_map::CssClassMap &class_map) {
  epub_css_class_map::CssClassMargins margins;
  if (epub_css_class_map::LookupMarginsForClassAttr(class_attr, class_map,
                                                    &margins)) {
    return margins.margin_top;
  }
  return {};
}

static book_xml_css_style_utils::MarginTopResult
LookupClassMarginBottom(const std::string &class_attr,
                        const epub_css_class_map::CssClassMap &class_map) {
  epub_css_class_map::CssClassMargins margins;
  if (epub_css_class_map::LookupMarginsForClassAttr(class_attr, class_map,
                                                    &margins)) {
    return margins.margin_bottom;
  }
  return {};
}

static book_xml_css_style_utils::MarginTopResult
LookupClassMarginLeft(const std::string &class_attr,
                      const epub_css_class_map::CssClassMap &class_map) {
  return epub_css_class_map::LookupMarginLeftForClassAttr(class_attr, class_map);
}

static book_xml_css_style_utils::MarginTopResult
LookupClassMarginRight(const std::string &class_attr,
                       const epub_css_class_map::CssClassMap &class_map) {
  return epub_css_class_map::LookupMarginRightForClassAttr(class_attr,
                                                           class_map);
}

static book_xml_css_style_utils::MarginTopResult
LookupClassTextIndent(const std::string &class_attr,
                      const epub_css_class_map::CssClassMap &class_map) {
  return epub_css_class_map::LookupTextIndentForClassAttr(class_attr, class_map);
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginLeftWithClass(const char **attr, const parsedata_t *p) {
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const auto r = book_xml_css_style_utils::ParseMarginLeft(attr[i + 1]);
        if (r.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
          return r;
      }
    }
  }
  const std::string cls = ExtractClassAttr(attr);
  return LookupClassMarginLeft(cls, p ? p->css_class_map
                                      : epub_css_class_map::CssClassMap{});
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginRightWithClass(const char **attr, const parsedata_t *p) {
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const auto r = book_xml_css_style_utils::ParseMarginRight(attr[i + 1]);
        if (r.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
          return r;
      }
    }
  }
  const std::string cls = ExtractClassAttr(attr);
  return LookupClassMarginRight(cls, p ? p->css_class_map
                                       : epub_css_class_map::CssClassMap{});
}

static book_xml_css_style_utils::MarginTopResult
ParseElementTextIndentWithClass(const char **attr, const parsedata_t *p) {
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const auto r = book_xml_css_style_utils::ParseTextIndent(attr[i + 1]);
        if (r.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
          return r;
      }
    }
  }
  const std::string cls = ExtractClassAttr(attr);
  return LookupClassTextIndent(cls, p ? p->css_class_map
                                      : epub_css_class_map::CssClassMap{});
}

static int
ResolveHorizontalMarginPx(const book_xml_css_style_utils::MarginTopResult &mtr,
                          int display_width) {
  using book_xml_css_style_utils::MarginTopResult;
  if (mtr.unit == MarginTopResult::Unit::None)
    return 0;
  if (mtr.unit == MarginTopResult::Unit::Percent)
    return (mtr.value * display_width) / 100;
  return mtr.value;
}

static u8
ParseElementTextTransform(const char **attr,
                           const epub_css_class_map::CssClassMap &class_map) {
  using book_xml_css_style_utils::TextTransform;
  u8 result = 0;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const TextTransform tt =
            book_xml_css_style_utils::ParseTextTransform(attr[i + 1]);
        if (tt != TextTransform::None)
          result = (u8)tt;
      } else if (AttrNameEquals(attr[i], "class")) {
        TextTransform tt;
        if (epub_css_class_map::LookupTextTransformForClassAttr(
                std::string(attr[i + 1]), class_map, &tt))
          result = (u8)tt;
      }
    }
  }
  return result;
}

static u8 ParseElementWhiteSpace(
    const char **attr, const epub_css_class_map::CssClassMap &class_map) {
  using book_xml_css_style_utils::WhiteSpaceMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        WhiteSpaceMode mode = WhiteSpaceMode::Normal;
        if (book_xml_css_style_utils::TryParseWhiteSpace(attr[i + 1], &mode))
          return (u8)mode + 1;
      } else if (AttrNameEquals(attr[i], "class")) {
        WhiteSpaceMode mode = WhiteSpaceMode::Normal;
        if (epub_css_class_map::LookupWhiteSpaceForClassAttr(
                std::string(attr[i + 1]), class_map, &mode)) {
          return (u8)mode + 1;
        }
      }
    }
  }
  return 0;
}

static book_xml_css_style_utils::WhiteSpaceMode
ResolveActiveWhiteSpace(const parsedata_t *p) {
  using book_xml_css_style_utils::WhiteSpaceMode;
  if (!p)
    return WhiteSpaceMode::Normal;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->style_white_space_stack[i] != 0)
      return (WhiteSpaceMode)(p->style_white_space_stack[i] - 1);
  }
  if (parse_in((parsedata_t *)p, TAG_PRE))
    return WhiteSpaceMode::PreWrap;
  return WhiteSpaceMode::Normal;
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopWithClass(const char **attr, const parsedata_t *p) {
  const book_xml_css_style_utils::MarginTopResult from_style =
      ParseElementMarginTopPx(attr);
  if (from_style.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
    return from_style;
  const std::string cls = ExtractClassAttr(attr);
  return LookupClassMarginTop(cls, p->css_class_map);
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginBottomWithClass(const std::string &last_style,
                                  const std::string &last_class,
                                  const epub_css_class_map::CssClassMap &class_map) {
  using book_xml_css_style_utils::MarginTopResult;
  const MarginTopResult from_style =
      book_xml_css_style_utils::ParseMarginBottom(last_style.c_str());
  if (from_style.unit != MarginTopResult::Unit::None)
    return from_style;
  return LookupClassMarginBottom(last_class, class_map);
}

static bool FindActiveBlockTextAlign(
    const parsedata_t *p, book_xml_css_style_utils::TextAlign *out) {
  if (!p || !out)
    return false;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->block_text_align_stack[i])
      continue;
    *out = (book_xml_css_style_utils::TextAlign)
               p->block_text_align_value_stack[i];
    return true;
  }
  return false;
}

static book_xml_css_style_utils::TextAlign
ResolveElementTextAlignWithClass(const std::string &style_attr,
                                 const std::string &class_attr,
                                 const parsedata_t *p,
                                 const epub_css_class_map::CssClassMap &class_map) {
  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  if (book_xml_css_style_utils::TryParseTextAlign(style_attr.c_str(), &align))
    return align;
  if (epub_css_class_map::LookupTextAlignForClassAttr(class_attr, class_map,
                                                      &align)) {
    return align;
  }
  if (FindActiveBlockTextAlign(p, &align))
    return align;
  return book_xml_css_style_utils::TextAlign::Left;
}

static void AppendParagraphAlignMarker(
    parsedata_t *p, book_xml_css_style_utils::TextAlign align) {
  if (!p)
    return;
  if (align == book_xml_css_style_utils::TextAlign::Center) {
    AppendParsedByte(p, TEXT_PARAGRAPH_CENTER);
  } else if (align == book_xml_css_style_utils::TextAlign::Right) {
    AppendParsedByte(p, TEXT_PARAGRAPH_RIGHT);
  } else {
    AppendParsedByte(p, TEXT_PARAGRAPH_LEFT);
  }
}

static bool StyleLooksDisplayBlock(const std::string &style_attr) {
  const std::string style_lc = ToLowerAsciiLocal(style_attr);
  return ContainsAsciiNoCase(style_lc, "display:block") ||
         ContainsAsciiNoCase(style_lc, "display: block");
}

static bool ElementCanCarryBlockTextAlign(const char *el,
                                          const std::string &style_attr) {
  return !strcmp(el, "body") || !strcmp(el, "div") ||
         !strcmp(el, "aside") || !strcmp(el, "blockquote") ||
         !strcmp(el, "caption") || !strcmp(el, "figure") ||
         AttrNameEquals(el, "section") || StyleLooksDisplayBlock(style_attr);
}

static void RestoreActiveBlockTextAlignMarker(parsedata_t *p) {
  if (!p)
    return;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->block_text_align_stack[i])
      continue;
    AppendParagraphAlignMarker(
        p, (book_xml_css_style_utils::TextAlign)
               p->block_text_align_value_stack[i]);
    return;
  }
  AppendParagraphAlignMarker(p, book_xml_css_style_utils::TextAlign::Left);
}

static void ConfigureBlockTextAlign(parsedata_t *p, const char *el,
                                    const char **attr) {
  if (!p || !el || p->stacksize == 0)
    return;

  const std::string style_attr = ExtractStyleAttr(attr);
  const std::string class_attr = ExtractClassAttr(attr);
  if (!ElementCanCarryBlockTextAlign(el, style_attr))
    return;

  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  const bool has_align =
      book_xml_css_style_utils::TryParseTextAlign(style_attr.c_str(), &align) ||
      epub_css_class_map::LookupTextAlignForClassAttr(class_attr,
                                                      p->css_class_map, &align);
  if (!has_align)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  p->block_text_align_stack[current] = true;
  p->block_text_align_value_stack[current] = (u8)align;
  AppendParagraphAlignMarker(p, align);
}

static void ApplyHeadingFontSize(parsedata_t *p, Text *ts, int heading_level,
                                 const std::string &style_attr,
                                 const std::string &class_attr) {
  if (!p || !ts || p->stacksize == 0)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  const int base_px = (int)ts->GetPixelSize();
  p->heading_saved_font_size_stack[current] = (u8)base_px;
  p->heading_font_size_emitted_stack[current] = false;

  const int heading_px = book_xml_parser_style_utils::ComputeHeadingFontSize(
      base_px, heading_level, style_attr, class_attr, p->css_class_map);
  if (heading_px == base_px)
    return;

  ts->SetPixelSize((u8)heading_px);
  AppendParsedByte(p, TEXT_FONT_SIZE);
  AppendParsedByte(p, (u32)heading_px);
  p->heading_font_size_emitted_stack[current] = true;
}

static void RestoreHeadingFontSize(parsedata_t *p, Text *ts) {
  if (!p || !ts || p->stacksize == 0)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  if (!p->heading_font_size_emitted_stack[current])
    return;

  ts->SetPixelSize(p->heading_saved_font_size_stack[current]);
  AppendParsedByte(p, TEXT_FONT_SIZE);
  AppendParsedByte(p, p->heading_saved_font_size_stack[current]);
  p->heading_font_size_emitted_stack[current] = false;
}

static int ResolveHeadingFontSizePx(parsedata_t *p, Text *ts, int heading_level,
                                    const std::string &style_attr,
                                    const std::string &class_attr) {
  if (!p || !ts)
    return 0;
  return book_xml_parser_style_utils::ComputeHeadingFontSize(
      (int)ts->GetPixelSize(), heading_level, style_attr, class_attr,
      p->css_class_map);
}

static int MeasureLineHeightForPixelSize(Text *ts, int pixel_size) {
  if (!ts || pixel_size <= 0)
    return 0;
  const u8 saved_px = ts->GetPixelSize();
  if ((int)saved_px == pixel_size)
    return ts->GetHeight();
  ts->SetPixelSize((u8)pixel_size);
  const int line_height = ts->GetHeight();
  ts->SetPixelSize(saved_px);
  return line_height;
}

static bool ShouldRenderHrRule(const std::string &style_attr,
                               const std::string &class_attr) {
  if (ContainsAsciiNoCase(class_attr, "transition"))
    return false;
  if (ContainsAsciiNoCase(style_attr, "border:none") ||
      ContainsAsciiNoCase(style_attr, "border: none") ||
      ContainsAsciiNoCase(style_attr, "border-top:none") ||
      ContainsAsciiNoCase(style_attr, "border-top: none") ||
      ContainsAsciiNoCase(style_attr, "border-bottom:none") ||
      ContainsAsciiNoCase(style_attr, "border-bottom: none")) {
    return false;
  }
  return true;
}

static bool ImagePathLooksLikeSvgWrapper(const std::string &path) {
  static const std::vector<u8> empty;
  return epub_image_utils::LooksLikeSvgWrapper(path, empty);
}

static const char *MarginUnitName(
    book_xml_css_style_utils::MarginTopResult::Unit unit) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  switch (unit) {
  case Unit::Px:
    return "px";
  case Unit::Percent:
    return "%";
  case Unit::None:
  default:
    return "none";
  }
}

static void LogResolvedBlockMargin(parsedata_t *p, const char *tag,
                                   const char *phase,
                                   const std::string &style_attr,
                                   const std::string &class_attr,
                                   const book_xml_css_style_utils::MarginTopResult &m,
                                   int line_h, int default_lf, int final_lf) {
#ifdef DSLIBRIS_DEBUG
  if (!p || !p->book || !p->book->GetStatusReporter())
    return;
  if (style_attr.empty() && class_attr.empty())
    return;
  DBG_LOGF_CAT(
      p->book->GetStatusReporter(), DBG_LEVEL_TRACE, DBG_CAT_EPUB,
      "EPUB: margin %s tag=%s path=%s style=\"%s\" class=\"%s\" unit=%s value=%d negative=%d line_h=%d default_lf=%d final_lf=%d screen=%d pen_y=%d",
      phase ? phase : "?", tag ? tag : "?", p->docpath.c_str(),
      style_attr.c_str(), class_attr.c_str(), MarginUnitName(m.unit), m.value,
      m.negative ? 1 : 0, line_h, default_lf, final_lf, p->screen, p->pen.y);
#else
  (void)p;
  (void)tag;
  (void)phase;
  (void)style_attr;
  (void)class_attr;
  (void)m;
  (void)line_h;
  (void)default_lf;
  (void)final_lf;
#endif
}

static void ParseElementStyleFlags(const char **attr, bool *bold_out,
                                   bool *italic_out, bool *underline_out,
                                   u8 *underline_style_out,
                                   bool *overline_out, bool *strikethrough_out,
                                   bool *superscript_out, bool *subscript_out,
                                   bool *no_underline_out,
                                   bool *reset_bold_out,
                                   bool *reset_italic_out) {
  if ((!bold_out && !italic_out && !underline_out && !overline_out &&
       !strikethrough_out &&
       !superscript_out && !subscript_out) ||
      !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "style")) {
      book_xml_css_style_utils::InlineStyleFlags flags{};
      book_xml_css_style_utils::ParseInlineStyleFlags(attr[i + 1], &flags);
      if (bold_out && flags.bold)
        *bold_out = true;
      if (italic_out && flags.italic)
        *italic_out = true;
      if (underline_out && flags.underline)
        *underline_out = true;
      if (underline_style_out && flags.underline)
        *underline_style_out = flags.underline_style;
      if (overline_out && flags.overline)
        *overline_out = true;
      if (strikethrough_out && flags.strikethrough)
        *strikethrough_out = true;
      if (superscript_out && flags.superscript)
        *superscript_out = true;
      if (subscript_out && flags.subscript)
        *subscript_out = true;
      if (no_underline_out && flags.no_underline)
        *no_underline_out = true;
      if (reset_bold_out && flags.reset_bold)
        *reset_bold_out = true;
      if (reset_italic_out && flags.reset_italic)
        *reset_italic_out = true;
    } else if (AttrNameEquals(attr[i], "class")) {
      ParseClassStyleFlags(attr[i + 1], bold_out, italic_out, underline_out,
                           underline_style_out, overline_out,
                           strikethrough_out, superscript_out, subscript_out);
    }
  }
}

static bool HasClassTokenNoCase(const char *class_name, const char *token) {
  if (!class_name || !token || !token[0])
    return false;
  const std::string class_lc = ToLowerAsciiLocal(class_name);
  const std::string token_lc = ToLowerAsciiLocal(token);
  return ContainsToken(class_lc, token_lc);
}

static void ParseInlineHiddenFlags(const char *style, bool *hidden_out) {
  if (!style || !hidden_out)
    return;
  const std::string style_lc = ToLowerAsciiLocal(style);

  if (ContainsAsciiNoCase(style_lc, "display:none") ||
      ContainsAsciiNoCase(style_lc, "display: none") ||
      ContainsAsciiNoCase(style_lc, "visibility:hidden") ||
      ContainsAsciiNoCase(style_lc, "visibility: hidden") ||
      ContainsAsciiNoCase(style_lc, "clip:rect(0,0,0,0)") ||
      ContainsAsciiNoCase(style_lc, "clip: rect(0, 0, 0, 0)") ||
      ContainsAsciiNoCase(style_lc, "clip-path:inset(50%)") ||
      ContainsAsciiNoCase(style_lc, "clip-path: inset(50%)")) {
    *hidden_out = true;
    return;
  }

  const bool tiny =
      (ContainsAsciiNoCase(style_lc, "width:1px") ||
       ContainsAsciiNoCase(style_lc, "width: 1px")) &&
      (ContainsAsciiNoCase(style_lc, "height:1px") ||
       ContainsAsciiNoCase(style_lc, "height: 1px"));
  const bool offscreen =
      ContainsAsciiNoCase(style_lc, "position:absolute") ||
      ContainsAsciiNoCase(style_lc, "position: absolute");
  const bool hidden_overflow =
      ContainsAsciiNoCase(style_lc, "overflow:hidden") ||
      ContainsAsciiNoCase(style_lc, "overflow: hidden");
  if (tiny && offscreen && hidden_overflow)
    *hidden_out = true;
}

static void ParseClassHiddenFlags(const char *class_name, bool *hidden_out) {
  if (!class_name || !hidden_out)
    return;
  if (HasClassTokenNoCase(class_name, "visually-hidden") ||
      HasClassTokenNoCase(class_name, "visuallyhidden") ||
      HasClassTokenNoCase(class_name, "sr-only") ||
      HasClassTokenNoCase(class_name, "screen-reader-text")) {
    *hidden_out = true;
  }
}

static bool AttrTruthyNoCase(const char *value) {
  if (!value || !value[0])
    return true;
  return EqualsAsciiNoCase(value, "1") || EqualsAsciiNoCase(value, "true") ||
         EqualsAsciiNoCase(value, "yes") ||
         EqualsAsciiNoCase(value, "hidden");
}

static void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  if (!hidden_out || !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (AttrNameEquals(name, "hidden")) {
      *hidden_out = true;
    } else if (AttrNameEquals(name, "aria-hidden")) {
      if (AttrTruthyNoCase(value))
        *hidden_out = true;
    } else if (value && value[0] && AttrNameEquals(name, "style")) {
      ParseInlineHiddenFlags(value, hidden_out);
    } else if (value && value[0] && AttrNameEquals(name, "class")) {
      ParseClassHiddenFlags(value, hidden_out);
    }
  }
}

static bool HasActiveStackBoldStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_bold_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_hidden_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackItalicStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_italic_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_underline_stack[i])
      return true;
  }
  return false;
}

static u8 ResolveActiveUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return UNDERLINE_STYLE_SOLID;
  for (int i = (int)p->stacksize - 1; i >= 0; i--) {
    if (p->style_underline_stack[i])
      return p->style_underline_style_stack[i];
  }
  for (int i = (int)p->stacksize - 1; i >= 0; i--) {
    if (p->stack[i] == TAG_UNDERLINE)
      return UNDERLINE_STYLE_SOLID;
  }
  return UNDERLINE_STYLE_SOLID;
}

static bool HasActiveStackOverlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_overline_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_strikethrough_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_superscript_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_subscript_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackNoUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_no_underline_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackResetBoldStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_reset_bold_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackResetItalicStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_reset_italic_stack[i])
      return true;
  }
  return false;
}

static bool HasActiveStackMonoStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_mono_stack[i])
      return true;
  }
  return false;
}

static void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight) {
  if (!p || !p->book || !p->ts)
    return;

  Text *ts = p->ts;
  const int leftBottomMargin = ts->margin.bottom;
  const int rightBottomMargin = MIN(ts->margin.bottom, 16);
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, leftBottomMargin,
          rightBottomMargin);
  int maxHeight = metrics.max_height;
  int bottomMargin = metrics.bottom_margin;
  if (!text_render_layout_utils::WouldOverflowReadingScreen(
          p->pen.y, lineheight, ts->linespacing, maxHeight, bottomMargin))
    return;

  p->perf_page_overflows++;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    page->start = p->pos;
    p->pos += p->buflen;
    page->end = p->pos;
    p->pagecount++;

    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + lineheight;
}

static void AdvanceParsedPageOnOverflowThunk(parsedata_t *p, int lineheight,
                                             void *ctx) {
  (void)ctx;
  AdvanceParsedPageOnOverflow(p, lineheight);
}

#ifdef DSLIBRIS_DEBUG
struct ChardataPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit ChardataPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~ChardataPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_chardata_calls++;
    parsedata->perf_chardata_ms += (u64)(osGetTime() - t_begin);
  }
};
#else
struct ChardataPerfScope {
  explicit ChardataPerfScope(parsedata_t *) {}
};
#endif

static void EmitFlowedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx,
    const book_xml_text_emit::FlowEmitMetrics &emit_metrics) {
  if (!p || !txt || txtlen == 0)
    return;

  std::vector<text_layout_utils::ShapedGlyph> run;
  bool has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &run, &has_rtl))
    return;

  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  if (has_rtl) {
    std::vector<uint32_t> run_cps;
    run_cps.reserve(run.size());
    for (size_t ci = 0; ci < run.size(); ci++)
      run_cps.push_back(run[ci].text.codepoint);
    text_bidi_utils::AnalyzeBidiRuns(run_cps.data(), run_cps.size(),
                                     &bidi_runs);
  }
  book_xml_text_emit::EmitFlowedShapedText(
      p, txt, run, has_rtl, bidi_runs, emit_metrics,
      AdvanceParsedPageOnOverflowThunk, NULL);
}

static void EmitPreformattedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx, int lineheight,
    int linespacing, bool allow_wrap) {
  if (!p || !txt || txtlen == 0)
    return;

  Text *ts = p->ts;
  if (!allow_wrap) {
    size_t offset = 0;
    while (offset < txtlen) {
      uint32_t cp = 0;
      const size_t step = text_unicode_utils::DecodeNextDisplayCodepoint(
          txt + offset, txtlen - offset, &cp);
      if (step == 0) {
        offset++;
        continue;
      }
      if (cp == '\r') {
        offset += step;
        continue;
      }
      if (cp == '\n') {
        AppendParsedByte(p, '\n');
        p->pen.x = ts->margin.left;
        p->pen.y += (lineheight + linespacing);
        p->linebegan = false;
        AdvanceParsedPageOnOverflow(p, lineheight);
        offset += step;
        continue;
      }

      AdvanceParsedPageOnOverflow(p, lineheight);
      book_xml_text_emit::AppendParsedCodepoints(p, txt + offset, step);
      p->pen.x += ts->GetAdvance(cp, measure_ctx.style);
      p->linebegan = true;
      offset += step;
    }
    return;
  }

  std::vector<text_layout_utils::ShapedGlyph> pre_run;
  bool pre_has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &pre_run, &pre_has_rtl)) {
    return;
  }

  std::vector<text_bidi_utils::BidiRun> pre_bidi_runs;
  if (pre_has_rtl) {
    std::vector<uint32_t> pre_cps;
    pre_cps.reserve(pre_run.size());
    for (size_t ci = 0; ci < pre_run.size(); ci++)
      pre_cps.push_back(pre_run[ci].text.codepoint);
    text_bidi_utils::AnalyzeBidiRuns(pre_cps.data(), pre_cps.size(),
                                     &pre_bidi_runs);
    if (book_xml_text_emit::DetectParagraphRTL(pre_run))
      AppendParsedByte(p, TEXT_PARAGRAPH_RTL);
    else
      AppendParsedByte(p, TEXT_PARAGRAPH_LTR);
  }

  const int max_pre_line_width =
      ts->display.width - ts->margin.right - ts->margin.left;
  size_t unit_index = 0;
  while (unit_index < pre_run.size()) {
    const text_layout_utils::ShapedGlyph &unit = pre_run[unit_index];
    if (unit.text.codepoint == '\r') {
      unit_index++;
      continue;
    }
    if (unit.text.codepoint == '\n') {
      AppendParsedByte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      AdvanceParsedPageOnOverflow(p, lineheight);
      unit_index++;
      continue;
    }

    text_layout_utils::LineBreakMeasureResult segment =
        text_layout_utils::FindPreformattedLineBreakAndMeasure(
            pre_run, unit_index, max_pre_line_width);
    size_t segment_end_index = segment.end_index;
    if (segment_end_index <= unit_index)
      segment_end_index = unit_index + 1;

    size_t segment_start = pre_run[unit_index].text.byte_offset;
    size_t segment_end =
        pre_run[segment_end_index - 1].text.byte_offset +
        pre_run[segment_end_index - 1].text.byte_length;
    const int advance = segment.width;

    if ((p->pen.x + advance) >= (ts->display.width - ts->margin.right)) {
      AppendParsedByte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      AdvanceParsedPageOnOverflow(p, lineheight);
    }

    if (pre_has_rtl) {
      AppendParsedByte(p, TEXT_RTL_LINE_PX);
      AppendParsedByte(p, (u32)advance);
      book_xml_text_emit::EmitBidiSegment(p, pre_run, unit_index,
                                          segment_end_index, pre_bidi_runs);
    } else {
      book_xml_text_emit::AppendParsedCodepoints(
          p, txt + segment_start, segment_end - segment_start);
    }
    p->pen.x += advance;
    p->linebegan = true;
    unit_index = segment_end_index;

    if (unit_index < pre_run.size() &&
        pre_run[unit_index].text.codepoint != '\n') {
      AppendParsedByte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      AdvanceParsedPageOnOverflow(p, lineheight);
    }
  }
}

static void EmitFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt,
                                  int txtlen) {
  if (!p || !txt || txtlen <= 0)
    return;

  Text *ts = p->ts;
  SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
  const u8 parse_text_style =
      book_xml_parser_style_utils::ResolveParsedTextStyle(p->bold, p->italic,
                                                          p->mono);
  const ParsedTextMeasureContext measure_ctx =
      MakeParsedTextMeasureContext(ts, p->bold, p->italic, p->mono);

  int lineheight = ts->GetHeight();
  int linespacing = ts->linespacing;
  int spaceadvance = ts->GetAdvance((u16)' ', parse_text_style);

  if (p->buflen == 0) {
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + lineheight;
    p->linebegan = false;
  }

  const book_xml_css_style_utils::WhiteSpaceMode white_space =
      ResolveActiveWhiteSpace(p);
  book_xml_text_emit::FlowEmitMetrics emit_metrics{};
  emit_metrics.display_width = ts->display.width;
  emit_metrics.margin_left = ts->margin.left + p->block_margin_left;
  emit_metrics.margin_right = ts->margin.right + p->block_margin_right;
  emit_metrics.lineheight = lineheight;
  emit_metrics.linespacing = linespacing;
  emit_metrics.spaceadvance = spaceadvance;

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap) {
    EmitPreformattedUtf8Segment(
        p, txt, (size_t)txtlen, measure_ctx, lineheight, linespacing,
        white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap);
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Nowrap) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            txt, (size_t)txtlen, white_space);
    if (!normalized.empty()) {
      book_xml_text_emit::AppendParsedCodepoints(
          p, normalized.c_str(), normalized.size());
      size_t offset = 0;
      while (offset < normalized.size()) {
        uint32_t cp = 0;
        const size_t step = text_unicode_utils::DecodeNextDisplayCodepoint(
            normalized.c_str() + offset, normalized.size() - offset, &cp);
        if (step == 0) {
          offset++;
          continue;
        }
        p->pen.x += ts->GetAdvance(cp, parse_text_style);
        offset += step;
      }
      p->linebegan = !normalized.empty();
    }
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::PreLine) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            txt, (size_t)txtlen, white_space);
    size_t start = 0;
    while (start <= normalized.size()) {
      const size_t nl = normalized.find('\n', start);
      const size_t end =
          (nl == std::string::npos) ? normalized.size() : nl;
      if (end > start) {
        EmitFlowedUtf8Segment(p, normalized.c_str() + start, end - start,
                              measure_ctx, emit_metrics);
      }
      if (nl == std::string::npos)
        break;
      AppendParsedByte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      AdvanceParsedPageOnOverflow(p, lineheight);
      start = nl + 1;
    }
    return;
  }

  EmitFlowedUtf8Segment(p, txt, (size_t)txtlen, measure_ctx, emit_metrics);
}

static void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts) {
  ApplyDeferredStyleSync(p, ts);
}

static std::string NormalizeDocPath(const std::string &path) {
  std::string in = path;
  std::replace(in.begin(), in.end(), '\\', '/');
  while (!in.empty() && in[0] == '/')
    in.erase(in.begin());

  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= in.size(); i++) {
    if (i == in.size() || in[i] == '/') {
      if (cur == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(in[i]);
    }
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i)
      out.push_back('/');
    out += parts[i];
  }
  return out;
}

static bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static bool PathLooksLikeTocDoc(const std::string &path) {
  if (path.empty())
    return false;
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return lower.find("toc") != std::string::npos ||
         lower.find("indice") != std::string::npos ||
         lower.find("index") != std::string::npos ||
         lower.find("contents") != std::string::npos ||
         lower.find("contenido") != std::string::npos ||
         lower.find("nav") != std::string::npos;
}

static bool DocLooksLikeTocDoc(const parsedata_t *p) {
  if (!p)
    return false;
  return PathLooksLikeTocDoc(p->docpath) || PathLooksLikeTocDoc(p->doc_title) ||
         PathLooksLikeTocDoc(p->doc_heading);
}

static std::string ResolveDocPath(const std::string &base_doc_path,
                                  const std::string &href) {
  if (href.empty())
    return "";
  if (href.find("://") != std::string::npos)
    return "";
  if (href.compare(0, 5, "data:") == 0)
    return "";

  std::string clean_href = href;
  size_t hash = clean_href.find('#');
  if (hash != std::string::npos)
    clean_href = clean_href.substr(0, hash);
  if (clean_href.empty())
    return "";

  if (!clean_href.empty() && clean_href[0] == '/')
    return NormalizeDocPath(clean_href);

  std::string base = base_doc_path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos)
    base = base.substr(0, slash + 1);
  else
    base.clear();

  return NormalizeDocPath(base + clean_href);
}

static std::string NormalizeFb2ChapterTitle(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty())
      out.push_back(' ');
    pending_space = false;
    out.push_back((char)c);
  }
  return out;
}

static void linefeed(parsedata_t *p) {
  AppendParsedByte(p, '\n');
  p->pen.x = MARGINLEFT;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

static bool blankline(parsedata_t *p) {
  // Was the preceding text a blank line?
  if (p->buflen < 3)
    return true;
  return (p->buf[p->buflen - 1] == '\n') && (p->buf[p->buflen - 2] == '\n');
}

static int CountTrailingLinefeeds(const parsedata_t *p) {
  if (!p || p->buflen <= 0)
    return 0;
  int count = 0;
  for (int i = p->buflen - 1; i >= 0; --i) {
    if (p->buf[i] != '\n')
      break;
    ++count;
  }
  return count;
}

static int EmitAdditionalTopLinefeeds(parsedata_t *p, int desired_lf) {
  if (!p || desired_lf <= 0)
    return 0;
  const int existing_lf = CountTrailingLinefeeds(p);
  const int add_lf = std::max(0, desired_lf - existing_lf);
  for (int i = 0; i < add_lf; i++)
    linefeed(p);
  return add_lf;
}

static bool GetTopActiveInlineLink(const parsedata_t *p, u16 *href_id_out) {
  if (!p || !href_id_out)
    return false;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->link_active_stack[i])
      continue;
    if (p->link_href_id_stack[i] == 0)
      continue;
    *href_id_out = p->link_href_id_stack[i];
    return true;
  }
  return false;
}

static void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  if (!p)
    return;
  u16 href_id = 0;
  if (!GetTopActiveInlineLink(p, &href_id))
    return;
  AppendParsedByte(p, TEXT_LINK_START);
  AppendParsedByte(p, (u32)href_id);
}

static void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  Text *ts = p->ts;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
}

static void ForcePageBreak(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;
  if (p->screen == 0 && p->buflen == 0)
    return;
  if (p->screen == 0)
    AdvanceParsedScreen(p);
  AdvanceParsedScreen(p);
}

static bool IsBlockLevelElement(const char *el) {
  return !strcmp(el, "p") || !strcmp(el, "div") ||
         !strcmp(el, "h1") || !strcmp(el, "h2") || !strcmp(el, "h3") ||
         !strcmp(el, "h4") || !strcmp(el, "h5") || !strcmp(el, "h6") ||
         !strcmp(el, "section") || !strcmp(el, "article") ||
         !strcmp(el, "aside") || !strcmp(el, "blockquote") ||
         !strcmp(el, "header") || !strcmp(el, "footer") ||
         !strcmp(el, "figure") || !strcmp(el, "dl") ||
         !strcmp(el, "dt") || !strcmp(el, "dd");
}

static void ResetCapturedTable(parsedata_t *p) {
  if (!p)
    return;
  p->table_in_header_section = false;
  p->table_in_caption = false;
  p->table_in_row = false;
  p->table_in_cell = false;
  p->table_current_cell_is_header = false;
  p->table_current_cell_is_row_header = false;
  p->table_caption_text.clear();
  p->table_current_cell_text.clear();
  p->table_header_cells.clear();
  p->table_current_row_cells.clear();
  p->table_current_row_header_flags.clear();
  p->table_body_rows.clear();
  p->table_body_row_header_flags.clear();
}

static void SetCurrentStackHidden(parsedata_t *p, bool hidden) {
  if (!p || p->stacksize == 0)
    return;
  p->style_hidden_stack[p->stacksize - 1] = hidden;
}

static std::string *GetActiveCapturedTableText(parsedata_t *p) {
  if (!p)
    return NULL;
  if (p->table_in_cell)
    return &p->table_current_cell_text;
  if (p->table_in_caption)
    return &p->table_caption_text;
  return NULL;
}

static void AppendCapturedTableSeparator(parsedata_t *p, char separator) {
  std::string *buffer = GetActiveCapturedTableText(p);
  if (!buffer || buffer->empty())
    return;
  const char last = (*buffer)[buffer->size() - 1];
  if (separator == '\n') {
    if (last != '\n')
      buffer->push_back('\n');
    return;
  }
  if (last != ' ' && last != '\n')
    buffer->push_back(' ');
}

static bool AttrScopeEqualsRow(const char **attr) {
  if (!attr)
    return false;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEquals(attr[i], "scope") &&
        EqualsAsciiNoCase(attr[i + 1], "row")) {
      return true;
    }
  }
  return false;
}

static void FinishCapturedTableCell(parsedata_t *p) {
  if (!p || !p->table_in_cell)
    return;
  p->table_current_row_cells.push_back(
      book_xml_table_utils::NormalizeTableCellText(p->table_current_cell_text));
  p->table_current_row_header_flags.push_back(
      p->table_current_cell_is_row_header ? 1
                                          : (p->table_current_cell_is_header ? 2
                                                                             : 0));
  p->table_current_cell_text.clear();
  p->table_current_cell_is_header = false;
  p->table_current_cell_is_row_header = false;
}

static void FinishCapturedTableRow(parsedata_t *p) {
  if (!p || p->table_current_row_cells.empty())
    return;

  bool all_header = p->table_in_header_section;
  if (!all_header) {
    all_header = true;
    for (size_t i = 0; i < p->table_current_row_header_flags.size(); i++) {
      if (p->table_current_row_header_flags[i] != 2) {
        all_header = false;
        break;
      }
    }
  }

  if (p->table_header_cells.empty() && all_header) {
    p->table_header_cells = p->table_current_row_cells;
  } else {
    p->table_body_rows.push_back(p->table_current_row_cells);
    p->table_body_row_header_flags.push_back(p->table_current_row_header_flags);
  }

  p->table_current_row_cells.clear();
  p->table_current_row_header_flags.clear();
}

static std::vector<std::string> BuildCapturedTableLines(const parsedata_t *p) {
  std::vector<std::string> lines;
  if (!p)
    return lines;

  book_xml_table_utils::TableRow header_row;
  book_xml_table_utils::TableRow *header_ptr = NULL;
  if (!p->table_header_cells.empty()) {
    for (size_t i = 0; i < p->table_header_cells.size(); i++) {
      book_xml_table_utils::TableCell cell;
      cell.text = p->table_header_cells[i];
      cell.is_header = true;
      cell.is_row_header = false;
      header_row.cells.push_back(cell);
    }
    header_ptr = &header_row;
  }

  std::vector<book_xml_table_utils::TableRow> rows;
  rows.reserve(p->table_body_rows.size());
  for (size_t r = 0; r < p->table_body_rows.size(); r++) {
    book_xml_table_utils::TableRow row;
    const std::vector<std::string> &src_cells = p->table_body_rows[r];
    const std::vector<u8> &src_flags = p->table_body_row_header_flags[r];
    for (size_t c = 0; c < src_cells.size(); c++) {
      book_xml_table_utils::TableCell cell;
      cell.text = src_cells[c];
      cell.is_header = false;
      cell.is_row_header = c < src_flags.size() && src_flags[c] == 1;
      row.cells.push_back(cell);
    }
    rows.push_back(row);
  }

  return book_xml_table_utils::BuildTableLines(p->table_caption_text, header_ptr,
                                               rows);
}

static void EmitCapturedTable(parsedata_t *p, Text *ts) {
  if (!p || !ts)
    return;
  const std::vector<std::string> lines = BuildCapturedTableLines(p);
  if (lines.empty())
    return;

  FlushInlineTailAndDeferredStyle(p, ts);
  if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
    linefeed(p);

  for (size_t i = 0; i < lines.size(); i++) {
    const std::string &line = lines[i];
    if (line.empty()) {
      linefeed(p);
      continue;
    }
    EmitFlowedFragmentRaw(p, line.c_str(), (int)line.size());
    linefeed(p);
  }
}

static bool HandleTableStart(parsedata_t *p, Text *ts, const char *el,
                             const char **attr) {
  if (!p || !el)
    return false;
  const bool entering_table = !strcmp(el, "table");
  const bool inside_table = parse_in(p, TAG_TABLE);
  if (!entering_table && !inside_table)
    return false;

  bool hidden = false;
  ParseElementHiddenFlags(attr, &hidden);

  if (entering_table) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
      linefeed(p);
    ResetCapturedTable(p);
    parse_push(p, TAG_TABLE);
    SetCurrentStackHidden(p, hidden);
    p->in_paragraph = false;
    p->paragraph_has_content = false;
    return true;
  }

  if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    SetCurrentStackHidden(p, hidden);
    p->table_in_caption = !hidden;
    return true;
  }
  if (!strcmp(el, "thead")) {
    parse_push(p, TAG_THEAD);
    SetCurrentStackHidden(p, hidden);
    p->table_in_header_section = !hidden;
    return true;
  }
  if (!strcmp(el, "tbody")) {
    parse_push(p, TAG_TBODY);
    SetCurrentStackHidden(p, hidden);
    return true;
  }
  if (!strcmp(el, "tr")) {
    parse_push(p, TAG_TR);
    SetCurrentStackHidden(p, hidden);
    p->table_in_row = !hidden;
    p->table_current_row_cells.clear();
    p->table_current_row_header_flags.clear();
    return true;
  }
  if (!strcmp(el, "th")) {
    parse_push(p, TAG_TH);
    SetCurrentStackHidden(p, hidden);
    p->table_in_cell = !hidden;
    p->table_current_cell_is_header = !hidden;
    p->table_current_cell_is_row_header = !hidden && AttrScopeEqualsRow(attr);
    p->table_current_cell_text.clear();
    return true;
  }
  if (!strcmp(el, "td")) {
    parse_push(p, TAG_TD);
    SetCurrentStackHidden(p, hidden);
    p->table_in_cell = !hidden;
    p->table_current_cell_is_header = false;
    p->table_current_cell_is_row_header = false;
    p->table_current_cell_text.clear();
    return true;
  }
  if (!strcmp(el, "br")) {
    parse_push(p, TAG_BR);
    SetCurrentStackHidden(p, hidden);
    if (!hidden)
      AppendCapturedTableSeparator(p, '\n');
    return true;
  }
  if (!strcmp(el, "p") || !strcmp(el, "div") || !strcmp(el, "li") ||
      !strcmp(el, "ul") || !strcmp(el, "ol") || !strcmp(el, "blockquote")) {
    parse_push(p, TAG_UNKNOWN);
    SetCurrentStackHidden(p, hidden);
    if (!hidden)
      AppendCapturedTableSeparator(p, ' ');
    return true;
  }
  if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
    parse_push(p, TAG_UNKNOWN);
    SetCurrentStackHidden(p, hidden);
    if (!hidden) {
      AppendCapturedTableSeparator(p, ' ');
      std::string *buffer = GetActiveCapturedTableText(p);
      if (buffer)
        buffer->append("[image]");
    }
    return true;
  }
  if (!strcmp(el, "script")) {
    parse_push(p, TAG_SCRIPT);
    SetCurrentStackHidden(p, hidden);
    return true;
  }
  if (!strcmp(el, "style")) {
    parse_push(p, TAG_STYLE);
    SetCurrentStackHidden(p, hidden);
    return true;
  }

  parse_push(p, TAG_UNKNOWN);
  SetCurrentStackHidden(p, hidden);
  return true;
}

static bool HandleTableEnd(parsedata_t *p, Text *ts, const char *el) {
  if (!p || !el || !parse_in(p, TAG_TABLE))
    return false;

  if (!strcmp(el, "th") || !strcmp(el, "td")) {
    FinishCapturedTableCell(p);
    p->table_in_cell = false;
    p->table_current_cell_is_header = false;
    p->table_current_cell_is_row_header = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "tr")) {
    if (p->table_in_cell)
      FinishCapturedTableCell(p);
    p->table_in_cell = false;
    FinishCapturedTableRow(p);
    p->table_in_row = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "thead")) {
    p->table_in_header_section = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "tbody")) {
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "caption")) {
    p->table_in_caption = false;
    parse_pop(p);
    return true;
  }
  if (!strcmp(el, "table")) {
    if (p->table_in_cell)
      FinishCapturedTableCell(p);
    if (p->table_in_row)
      FinishCapturedTableRow(p);
    EmitCapturedTable(p, ts);
    ResetCapturedTable(p);
    parse_pop(p);
    return true;
  }

  parse_pop(p);
  return true;
}

} // namespace

namespace xml::book::metadata {

std::string title;

void start(void *userdata, const char *el, const char **attr) {
  //! Expat callback, when entering an element.
  //! For finding book title only.

  if (!strcmp(el, "title")) {
    parse_push((parsedata_t *)userdata, TAG_TITLE);
  }
}

void chardata(void *userdata, const char *txt, int txtlen) {
  //! Expat callback, when in char data for element.
  //! For finding book title only.

  if (!parse_in((parsedata_t *)userdata, TAG_TITLE))
    return;
  title = txt;
}

void end(void *userdata, const char *el) {
  //! Expat callback, when exiting an element.
  //! For finding book title only.

  parsedata_t *data = (parsedata_t *)userdata;
  if (!strcmp(el, "title"))
    data->book->SetTitle(title.c_str());
  if (!strcmp(el, "head"))
    data->status = 1; // done.
  parse_pop(data);
}

} // namespace xml::book::metadata

namespace xml::book {

void chardata(void *data, const XML_Char *txt, int txtlen);

void instruction(void *data, const char *target, const char *pidata) {}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

  if (book_xml_hidden_utils::IsCosmeticPageBreakElement(attr)) {
    parse_push(p, TAG_UNKNOWN);
    SetCurrentStackHidden(p, true);
    return;
  }

  if (p->fb2_mode && parse_in(p, TAG_BODY)) {
    if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth < 31)
        p->fb2_section_depth++;
      if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
        p->fb2_section_has_chapter[p->fb2_section_depth] = false;
    } else if (XmlNameEquals(el, "title") && p->fb2_section_depth > 0) {
      p->fb2_title_depth++;
      if (p->fb2_title_depth == 1 && p->fb2_title_capture_depth == 0 &&
          p->fb2_section_depth < 32 &&
          !p->fb2_section_has_chapter[p->fb2_section_depth]) {
        p->fb2_title_capture_depth = p->fb2_section_depth;
        p->fb2_title_text.clear();
      }
    }
  }

  // Register named anchors while parsing EPUB documents so TOC hrefs with
  // fragments (#id) can jump to the closest real page instead of chapter start.
  if (p->book && !p->docpath.empty() && attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (XmlNameEquals(attr[i], "id") || XmlNameEquals(attr[i], "name")) {
        p->book->AddChapterAnchor(p->docpath, attr[i + 1]);
      }
    }
  }

  if (HandleTableStart(p, ts, el, attr))
    return;

  if (IsBlockLevelElement(el)) {
    const char *el_style = NULL;
    const char *el_class = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (AttrNameEquals(attr[i], "style"))
        el_style = attr[i + 1];
      else if (AttrNameEquals(attr[i], "class"))
        el_class = attr[i + 1];
    }
    if ((book_xml_css_style_utils::HasPageBreakInsideAvoid(el_style) ||
         epub_css_class_map::LookupPageBreakInsideAvoidForClassAttr(
             el_class ? std::string(el_class) : std::string(),
             p->css_class_map)) &&
        p->buflen > 0 && !blankline(p)) {
      ForcePageBreak(p);
    }
    if (book_xml_css_style_utils::HasPageBreakBefore(el_style) ||
        epub_css_class_map::LookupPageBreakBeforeForClassAttr(
            el_class ? std::string(el_class) : std::string(),
            p->css_class_map)) {
      ForcePageBreak(p);
    }
  }

  if (!strcmp(el, "html"))
    parse_push(p, TAG_HTML);
  else if (!strcmp(el, "aside")) {
    parse_push(p, TAG_ASIDE);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "blockquote")) {
    parse_push(p, TAG_BLOCKQUOTE);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "dd")) {
    parse_push(p, TAG_DD);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    if (!blankline(p))
      linefeed(p);
    const int leading_spaces = book_xml_block_utils::GetLeadingSpaceCount(TAG_DD);
    for (int i = 0; i < leading_spaces; i++) {
      AppendParsedByte(p, ' ');
      p->pen.x += ts->GetAdvance(' ');
    }
  }
  else if (!strcmp(el, "body"))
    parse_push(p, TAG_BODY);
  else if (!strcmp(el, "div")) {
    parse_push(p, TAG_DIV);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
  }
  else if (!strcmp(el, "dt")) {
    parse_push(p, TAG_DT);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
  }
  else if (!strcmp(el, "figure")) {
    parse_push(p, TAG_FIGURE);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    if (!blankline(p))
      linefeed(p);
  }
  else if (!strcmp(el, "h1")) {
    const std::string heading_style = ExtractStyleAttr(attr);
    const std::string heading_class = ExtractClassAttr(attr);
    const int heading_px =
        ResolveHeadingFontSizePx(p, ts, 1, heading_style, heading_class);
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    const text_render_layout_utils::ReadingScreenMetrics metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
            MIN(ts->margin.bottom, 16));
    req.screen_height = metrics.max_height;
    req.bottom_margin = metrics.bottom_margin;
    req.line_height = MeasureLineHeightForPixelSize(ts, heading_px);
    req.linespacing = ts->linespacing;
    req.heading_level = 1;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
    parse_push(p, TAG_H1);
    p->last_h1_style = heading_style;
    p->last_h1_class = heading_class;
    ApplyHeadingFontSize(p, ts, 1, p->last_h1_style, p->last_h1_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h1_style,
                                            p->last_h1_class, p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, p);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    const int line_h = ts->GetHeight() + ts->linespacing;
    const int default_lf = 1;
    const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
        default_lf, mtr, line_h);
    LogResolvedBlockMargin(p, "h1", "top", p->last_h1_style,
                           p->last_h1_class, mtr, line_h, default_lf,
                           lf_count);
    EmitAdditionalTopLinefeeds(p, lf_count);
  } else if (!strcmp(el, "h2")) {
    const std::string heading_style = ExtractStyleAttr(attr);
    const std::string heading_class = ExtractClassAttr(attr);
    const int heading_px =
        ResolveHeadingFontSizePx(p, ts, 2, heading_style, heading_class);
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    const text_render_layout_utils::ReadingScreenMetrics metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
            MIN(ts->margin.bottom, 16));
    req.screen_height = metrics.max_height;
    req.bottom_margin = metrics.bottom_margin;
    req.line_height = MeasureLineHeightForPixelSize(ts, heading_px);
    req.linespacing = ts->linespacing;
    req.heading_level = 2;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
    parse_push(p, TAG_H2);
    p->last_h2_style = heading_style;
    p->last_h2_class = heading_class;
    ApplyHeadingFontSize(p, ts, 2, p->last_h2_style, p->last_h2_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h2_style,
                                            p->last_h2_class, p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, p);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    const int line_h = ts->GetHeight() + ts->linespacing;
    const int default_lf = 1;
    const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
        default_lf, mtr, line_h);
    LogResolvedBlockMargin(p, "h2", "top", p->last_h2_style,
                           p->last_h2_class, mtr, line_h, default_lf,
                           lf_count);
    EmitAdditionalTopLinefeeds(p, lf_count);
  } else if (!strcmp(el, "h3")) {
    const std::string heading_style = ExtractStyleAttr(attr);
    const std::string heading_class = ExtractClassAttr(attr);
    const int heading_px =
        ResolveHeadingFontSizePx(p, ts, 3, heading_style, heading_class);
    heading_layout::KeepWithNextRequest req{};
    req.pen_y = p->pen.y;
    const text_render_layout_utils::ReadingScreenMetrics metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
            MIN(ts->margin.bottom, 16));
    req.screen_height = metrics.max_height;
    req.bottom_margin = metrics.bottom_margin;
    req.line_height = MeasureLineHeightForPixelSize(ts, heading_px);
    req.linespacing = ts->linespacing;
    req.heading_level = 3;
    if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
      AdvanceParsedScreen(p);
    parse_push(p, TAG_H3);
    p->last_h_style = heading_style;
    p->last_h_class = heading_class;
    ApplyHeadingFontSize(p, ts, 3, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, p);
      p->block_margin_left = ResolveHorizontalMarginPx(
          ParseElementMarginLeftWithClass(attr, p), ts->display.width);
      p->block_margin_right = ResolveHorizontalMarginPx(
          ParseElementMarginRightWithClass(attr, p), ts->display.width);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = 1;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h3", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      EmitAdditionalTopLinefeeds(p, lf_count);
    }
  } else if (!strcmp(el, "h4")) {
    parse_push(p, TAG_H4);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 4, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, p);
      p->block_margin_left = ResolveHorizontalMarginPx(
          ParseElementMarginLeftWithClass(attr, p), ts->display.width);
      p->block_margin_right = ResolveHorizontalMarginPx(
          ParseElementMarginRightWithClass(attr, p), ts->display.width);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 1 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h4", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      EmitAdditionalTopLinefeeds(p, lf_count);
    }
  } else if (!strcmp(el, "h5")) {
    parse_push(p, TAG_H5);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 5, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, p);
      p->block_margin_left = ResolveHorizontalMarginPx(
          ParseElementMarginLeftWithClass(attr, p), ts->display.width);
      p->block_margin_right = ResolveHorizontalMarginPx(
          ParseElementMarginRightWithClass(attr, p), ts->display.width);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 1 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h5", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      EmitAdditionalTopLinefeeds(p, lf_count);
    }
  } else if (!strcmp(el, "h6")) {
    parse_push(p, TAG_H6);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 6, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, p);
      p->block_margin_left = ResolveHorizontalMarginPx(
          ParseElementMarginLeftWithClass(attr, p), ts->display.width);
      p->block_margin_right = ResolveHorizontalMarginPx(
          ParseElementMarginRightWithClass(attr, p), ts->display.width);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 1 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h6", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      EmitAdditionalTopLinefeeds(p, lf_count);
    }
  } else if (!strcmp(el, "head"))
    parse_push(p, TAG_HEAD);
  else if (!strcmp(el, "ol"))
    parse_push(p, TAG_OL);
  else if (!strcmp(el, "p")) {
    parse_push(p, TAG_P);
    p->in_paragraph = true;
    p->paragraph_has_content = false;
    p->text_transform_word_start = true;
    p->last_p_style = ExtractStyleAttr(attr);
    p->last_p_class = ExtractClassAttr(attr);
    const book_xml_css_style_utils::TextAlign align =
        ResolveElementTextAlignWithClass(p->last_p_style, p->last_p_class,
                                         p, p->css_class_map);
    AppendParagraphAlignMarker(p, align);
    const bool inside_list_item = book_xml_list_utils::IsInsideListItem(p);
    const bool tight_list_paragraph =
        book_xml_list_utils::HasPendingListItemContent(p);
    const bool tight_block_paragraph = ParseInAnyEasyParagraphTightBlock(p);
    const bool can_apply_top_margin =
        !tight_list_paragraph && !tight_block_paragraph;
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, p);
    const book_xml_css_style_utils::MarginTopResult text_indent_mtr =
        ParseElementTextIndentWithClass(attr, p);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    const int line_h = ts->GetHeight() + ts->linespacing;
    if (can_apply_top_margin) {
      const int default_lf = p->book->GetParagraphSpacing();
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "p", "top", p->last_p_style,
                             p->last_p_class, mtr, line_h, default_lf,
                             lf_count);
      EmitAdditionalTopLinefeeds(p, lf_count);
      if (!inside_list_item && !parse_in(p, TAG_DD)) {
        int indent_spaces = 0;
        using book_xml_css_style_utils::MarginTopResult;
        if (text_indent_mtr.unit != MarginTopResult::Unit::None &&
            !text_indent_mtr.negative) {
          const int sa = ts->GetAdvance(' ');
          if (sa > 0)
            indent_spaces = text_indent_mtr.value / sa;
        } else if (text_indent_mtr.unit == MarginTopResult::Unit::None) {
          indent_spaces = p->book->GetParagraphIndent();
        }
        for (int i = 0; i < indent_spaces; i++) {
          AppendParsedByte(p, ' ');
          p->pen.x += ts->GetAdvance(' ');
        }
      }
    } else {
      const char *phase = "top-skipped";
      if (tight_list_paragraph)
        phase = "top-skipped-tight-list";
      else if (tight_block_paragraph)
        phase = "top-skipped-tight-block";
      LogResolvedBlockMargin(p, "p", phase, p->last_p_style, p->last_p_class,
                             mtr, line_h, 0, 0);
    }
  } else if (!strcmp(el, "hr")) {
    parse_push(p, TAG_UNKNOWN);
    p->last_hr_style = ExtractStyleAttr(attr);
    p->last_hr_class = ExtractClassAttr(attr);
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, p);
    p->block_margin_left = ResolveHorizontalMarginPx(
        ParseElementMarginLeftWithClass(attr, p), ts->display.width);
    p->block_margin_right = ResolveHorizontalMarginPx(
        ParseElementMarginRightWithClass(attr, p), ts->display.width);
    const int line_h = ts->GetHeight() + ts->linespacing;
    const int default_lf = !blankline(p) ? 1 : 0;
    const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
        default_lf, mtr, line_h);
    LogResolvedBlockMargin(p, "hr", "top", p->last_hr_style,
                           p->last_hr_class, mtr, line_h, default_lf,
                           lf_count);
    EmitAdditionalTopLinefeeds(p, lf_count);
    if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
      AppendParsedByte(p, TEXT_HR);
      // The renderer calls PrintNewLine() for TEXT_HR, advancing pen.y by one
      // line. Mirror that here so the parser's overflow tracking stays in sync.
      p->pen.y += ts->GetHeight() + ts->linespacing;
      p->pen.x = ts->margin.left;
      p->linebegan = false;
    }
  } else if (!strcmp(el, "pre")) {
    parse_push(p, TAG_PRE);
    p->preformatted_wrap_enabled = true;
    AppendParsedByte(p, TEXT_PRE_ON);
    if (!p->mono) {
      AppendParsedByte(p, TEXT_MONO_ON);
      p->mono = true;
      SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
    }
  } else if (!strcmp(el, "li")) {
    parse_push(p, TAG_LI);
    book_xml_list_utils::MarkCurrentListItemPending(p, true);
    const context_t active_list = book_xml_list_utils::GetActiveListContext(p);
    // HasSuppressedListMarkerContext checks ancestor elements (e.g. ol.classname).
    // ParseListMarkerHiddenCssClass checks the <li> element's own class
    // attribute, which ConfigureElementListSemantics hasn't processed yet.
    const bool suppress_marker =
        book_xml_list_utils::HasSuppressedListMarkerContext(p) ||
        book_xml_list_utils::ParseListMarkerHiddenCssClass(p, attr);
    if (active_list == TAG_UL || active_list == TAG_OL) {
      if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
        linefeed(p);
      if (!suppress_marker) {
        if (active_list == TAG_UL) {
          AppendParsedByte(p, 0x2022); // bullet '•'
          p->pen.x += ts->GetAdvance(0x2022) + ts->GetAdvance(' ');
        } else {
          const std::string marker = book_xml_list_utils::BuildOrderedListMarker(
              book_xml_list_utils::AdvanceOrderedListOrdinal(p),
              book_xml_list_utils::GetActiveOrderedListStyle(p));
          for (size_t i = 0; i < marker.size(); i++) {
            AppendParsedByte(p, (u32)(unsigned char)marker[i]);
            p->pen.x += ts->GetAdvance((u32)(unsigned char)marker[i]);
          }
          p->pen.x += ts->GetAdvance(' ');
        }
        AppendParsedByte(p, ' ');
        p->linebegan = true;
        p->strip_leading_list_marker = true;
      }
    }
  } else if (!strcmp(el, "script"))
    parse_push(p, TAG_SCRIPT);
  else if (!strcmp(el, "style"))
    parse_push(p, TAG_STYLE);
  else if (XmlNameEquals(el, "title"))
    parse_push(p, TAG_TITLE);
  else if (!strcmp(el, "ul"))
    parse_push(p, TAG_UL);
  else if (!strcmp(el, "strong") || !strcmp(el, "b")) {
    parse_push(p, TAG_STRONG);
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
  } else if (!strcmp(el, "em") || !strcmp(el, "i")) {
    parse_push(p, TAG_EM);
    AppendParsedByte(p, TEXT_ITALIC_ON);
    p->italic = true;
    SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
  } else if (!strcmp(el, "u") || !strcmp(el, "ins")) {
    parse_push(p, TAG_UNDERLINE);
    if (!p->underline) {
      AppendParsedByte(p, TEXT_UNDERLINE_ON);
      p->underline = true;
      p->underline_style = UNDERLINE_STYLE_SOLID;
      book_xml_parser_style_utils::EmitUnderlineStyleMarker(
          p, p->underline_style);
    }
  } else if (!strcmp(el, "strike") || !strcmp(el, "s") ||
             !strcmp(el, "del")) {
    parse_push(p, TAG_STRIKETHROUGH);
    if (!p->strikethrough) {
      AppendParsedByte(p, TEXT_STRIKETHROUGH_ON);
      p->strikethrough = true;
    }
  } else if (!strcmp(el, "sup")) {
    parse_push(p, TAG_SUPERSCRIPT);
    if (!p->superscript) {
      AppendParsedByte(p, TEXT_SUPERSCRIPT_ON);
      p->superscript = true;
    }
  } else if (!strcmp(el, "sub")) {
    parse_push(p, TAG_SUBSCRIPT);
    if (!p->subscript) {
      AppendParsedByte(p, TEXT_SUBSCRIPT_ON);
      p->subscript = true;
    }
  } else if (!strcmp(el, "code") || !strcmp(el, "tt") ||
             !strcmp(el, "kbd") || !strcmp(el, "samp")) {
    parse_push(p, TAG_CODE);
    if (!p->mono) {
      AppendParsedByte(p, TEXT_MONO_ON);
      p->mono = true;
      SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
    }
  } else if (!strcmp(el, "a")) {
    parse_push(p, TAG_ANCHOR);
    const u8 current = (u8)(p->stacksize - 1);
    p->link_active_stack[current] = false;
    p->link_href_id_stack[current] = 0;
    const char *href = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (XmlNameEquals(attr[i], "href")) {
        href = attr[i + 1];
        break;
      }
    }
    const std::string resolved_href =
        (href && *href)
            ? inline_link_utils::ResolveInternalHref(p->docpath, href)
            : std::string();
    if (!resolved_href.empty() && p->book) {
      const u16 href_id = p->book->RegisterInlineLinkHref(resolved_href);
      if (href_id != 0) {
        p->link_active_stack[current] = true;
        p->link_href_id_stack[current] = href_id;
        AppendParsedByte(p, TEXT_LINK_START);
        AppendParsedByte(p, (u32)href_id);
      }
    }
  } else if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
    parse_push(p, TAG_UNKNOWN);

    const char *src = NULL;
    const char *img_style = NULL;
    const char *img_width_attr = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (XmlNameEquals(attr[i], "src") || XmlNameEquals(attr[i], "href"))
        src = attr[i + 1];
      else if (AttrNameEquals(attr[i], "style"))
        img_style = attr[i + 1];
      else if (AttrNameEquals(attr[i], "width"))
        img_width_attr = attr[i + 1];
    }

    if (img_style) {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_css_style_utils::ParseMarginTop(img_style);
      const int default_lf = !blankline(p) ? 1 : 0;
      const int lf_count =
          book_xml_parser_style_utils::ResolveBlockTopLinefeeds(default_lf,
                                                                mtr, line_h);
      for (int i = 0; i < lf_count; i++)
        linefeed(p);
    }

    std::string resolved;
    if (src && *src) {
      std::string raw_src(src);
      if (!raw_src.empty() && raw_src[0] == '#') {
        // FB2 inline binary reference (<image href="#id">).
        resolved = "fb2:" + raw_src.substr(1);
      } else {
        resolved = ResolveDocPath(p->docpath, raw_src);
      }
    }

    if (!resolved.empty() && p->book) {
      u16 image_id = p->book->RegisterInlineImage(resolved);
      const int text_w = ts->display.width - ts->margin.left - ts->margin.right;
      const int author_max_w =
          ParseImgWidthPx(img_width_attr, img_style, text_w, ts->GetHeight());
      if (author_max_w > 0)
        p->book->SetInlineImageAuthorMaxWidth(image_id, author_max_w);
      InlineImageLayoutPlan image_plan{};
      const bool leading_paragraph_image =
          p->in_paragraph && !p->paragraph_has_content;
      const InlineImageContext image_context =
          leading_paragraph_image ? INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH
                                  : INLINE_IMAGE_CONTEXT_DEFAULT;
      p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x,
                                     p->pen.y, p->linebegan,
                                     image_context, &image_plan);

      // If the image can't be decoded (e.g. a pure SVG vector without an
      // embedded raster), PAGE mode would consume a full screen worth of space
      // while drawing nothing. Emit a text placeholder instead.
      InlineImageMetadata img_meta{};
      p->book->GetInlineImageMetadata(image_id, &img_meta);
      if (!img_meta.ok && image_plan.mode == INLINE_IMAGE_LAYOUT_PAGE &&
          ImagePathLooksLikeSvgWrapper(resolved)) {
        const char *fallback = "[illustration]";
        if (!blankline(p))
          linefeed(p);
        chardata(p, fallback, (int)strlen(fallback));
        linefeed(p);
        return;
      }

      // Mirror the renderer so pagination and draw agree on where the image
      // starts and how much space it consumes.
      if (image_plan.advance_before)
        AdvanceParsedScreen(p);
      if (image_plan.line_break_before && p->linebegan)
        linefeed(p);

      // The token stays format-agnostic; parser and renderer now derive the
      // concrete inline/band/page behavior from the same layout planner.
      if (leading_paragraph_image)
        AppendParsedByte(p, TEXT_IMAGE_LEADING_PARAGRAPH);
      AppendParsedByte(p, TEXT_IMAGE);
      AppendParsedByte(p, (u32)image_id);

      switch (image_plan.mode) {
      case INLINE_IMAGE_LAYOUT_INLINE:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        p->pen.x += image_plan.draw_width + ts->GetAdvance(' ');
        p->linebegan = true;
        break;

      case INLINE_IMAGE_LAYOUT_BAND:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        // Band images are block-level: following text resumes below, while
        // consecutive images may stack only if there is no text between them.
        p->pen.x = ts->margin.left;
        p->pen.y += image_plan.vertical_space_after_draw;
        p->linebegan = false;
        AdvanceParsedPageOnOverflow(p, ts->GetHeight());
        if (img_style) {
          const int line_h = ts->GetHeight() + ts->linespacing;
          const book_xml_css_style_utils::MarginTopResult mbr =
              book_xml_css_style_utils::ParseMarginBottom(img_style);
          const int lf_count =
              book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                  0, mbr, line_h);
          for (int i = 0; i < lf_count; i++)
            linefeed(p);
        }
        break;

      case INLINE_IMAGE_LAYOUT_PAGE:
      default:
        if (p->in_paragraph)
          p->paragraph_has_content = true;
        if (p->screen == 1) {
          AdvanceParsedScreen(p);
        } else {
          p->screen = 1;
          p->pen.x = ts->margin.left;
          p->pen.y = ts->margin.top + ts->GetHeight();
          p->linebegan = false;
        }
        break;
      }
    } else {
      // Keep a lightweight fallback marker when src cannot be resolved.
      const char *fallback = "[illustration]";
      if (!blankline(p))
        linefeed(p);
      chardata(p, fallback, (int)strlen(fallback));
      linefeed(p);
    }
  } else if (XmlNameEquals(el, "binary")) {
    parse_push(p, TAG_UNKNOWN);

    p->collecting_fb2_binary = false;
    p->fb2_binary_too_large = false;
    p->fb2_binary_id.clear();
    p->fb2_binary_data.clear();

    const char *id = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (XmlNameEquals(attr[i], "id")) {
        id = attr[i + 1];
      }
    }

    if (id && *id && p->book) {
      p->collecting_fb2_binary = true;
      p->fb2_binary_id = id;
      if (!p->fb2_binary_id.empty() && p->fb2_binary_id[0] == '#')
        p->fb2_binary_id.erase(0, 1);
    }
  } else
    parse_push(p, TAG_UNKNOWN);

  ConfigureBlockTextAlign(p, el, attr);

  // CSS-based emphasis fallback for EPUBs that do not use semantic tags.
  if (parse_in(p, TAG_BODY) && p->stacksize > 0) {
    bool style_bold = false;
    bool style_italic = false;
    bool style_underline = false;
    u8 style_underline_style = UNDERLINE_STYLE_SOLID;
    bool style_overline = false;
    bool style_strikethrough = false;
    bool style_superscript = false;
    bool style_subscript = false;
    bool style_no_underline = false;
    bool style_reset_bold = false;
    bool style_reset_italic = false;
    bool style_hidden = false;
    ParseElementStyleFlags(attr, &style_bold, &style_italic, &style_underline,
                           &style_underline_style, &style_overline,
                           &style_strikethrough,
                           &style_superscript,
                           &style_subscript,
                           &style_no_underline,
                           &style_reset_bold,
                           &style_reset_italic);
    // Also check CSS class map for vertical-align, underline reset, and bold/italic reset.
    {
      const std::string cls = ExtractClassAttr(attr);
      if (!style_superscript || !style_subscript)
        epub_css_class_map::LookupSuperSubForClassAttr(cls, p->css_class_map,
                                                       &style_superscript,
                                                       &style_subscript);
      if (!style_no_underline)
        style_no_underline = epub_css_class_map::LookupNoUnderlineForClassAttr(
            cls, p->css_class_map);
      if (!style_reset_bold)
        style_reset_bold = epub_css_class_map::LookupResetBoldForClassAttr(
            cls, p->css_class_map);
      if (!style_reset_italic)
        style_reset_italic = epub_css_class_map::LookupResetItalicForClassAttr(
            cls, p->css_class_map);
    }
    ParseElementHiddenFlags(attr, &style_hidden);

    const u8 current = (u8)(p->stacksize - 1);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
    p->style_bold_stack[current] = style_bold;
    p->style_italic_stack[current] = style_italic;
    p->style_underline_stack[current] = style_underline;
    p->style_underline_style_stack[current] = style_underline_style;
    p->style_overline_stack[current] = style_overline;
    p->style_strikethrough_stack[current] = style_strikethrough;
    p->style_superscript_stack[current] = style_superscript;
    p->style_subscript_stack[current] = style_subscript;
    p->style_hidden_stack[current] = style_hidden;
    p->style_no_underline_stack[current] = style_no_underline;
    p->style_reset_bold_stack[current] = style_reset_bold;
    p->style_reset_italic_stack[current] = style_reset_italic;
    p->style_text_transform_stack[current] =
        ParseElementTextTransform(attr, p->css_class_map);
    p->style_white_space_stack[current] =
        ParseElementWhiteSpace(attr, p->css_class_map);

    bool style_changed = false;
    if (style_bold && !style_reset_bold && !p->bold) {
      AppendParsedByte(p, TEXT_BOLD_ON);
      p->pos++;
      p->bold = true;
      style_changed = true;
    }
    if (style_reset_bold && p->bold) {
      AppendParsedByte(p, TEXT_BOLD_OFF);
      p->bold = false;
      style_changed = true;
    }
    if (style_italic && !style_reset_italic && !p->italic) {
      AppendParsedByte(p, TEXT_ITALIC_ON);
      p->italic = true;
      style_changed = true;
    }
    if (style_reset_italic && p->italic) {
      AppendParsedByte(p, TEXT_ITALIC_OFF);
      p->italic = false;
      style_changed = true;
    }
    if (style_no_underline && p->underline) {
      AppendParsedByte(p, TEXT_UNDERLINE_OFF);
      p->underline = false;
      p->underline_style = UNDERLINE_STYLE_SOLID;
      style_changed = true;
    }
    if (style_underline && !style_no_underline && !p->underline) {
      AppendParsedByte(p, TEXT_UNDERLINE_ON);
      p->underline = true;
      p->underline_style = style_underline_style;
      book_xml_parser_style_utils::EmitUnderlineStyleMarker(
          p, p->underline_style);
      style_changed = true;
    } else if (style_underline && p->underline &&
               p->underline_style != style_underline_style) {
      p->underline_style = style_underline_style;
      book_xml_parser_style_utils::EmitUnderlineStyleMarker(
          p, p->underline_style);
    }
    if (style_overline && !p->overline) {
      AppendParsedByte(p, TEXT_OVERLINE_ON);
      p->overline = true;
      style_changed = true;
    }
    if (style_strikethrough && !p->strikethrough) {
      AppendParsedByte(p, TEXT_STRIKETHROUGH_ON);
      p->strikethrough = true;
      style_changed = true;
    }
    if (style_superscript && !p->superscript) {
      AppendParsedByte(p, TEXT_SUPERSCRIPT_ON);
      p->superscript = true;
      style_changed = true;
    }
    if (style_subscript && !p->subscript) {
      AppendParsedByte(p, TEXT_SUBSCRIPT_ON);
      p->subscript = true;
      style_changed = true;
    }
    if (style_changed)
      SyncParsedTextStyle(ts, p->bold, p->italic, p->mono);
  }

  if (IsBlockLevelElement(el) && p->stacksize > 0) {
    const char *el_style = NULL;
    const char *el_class = NULL;
    for (int i = 0; attr && attr[i]; i += 2) {
      if (AttrNameEquals(attr[i], "style"))
        el_style = attr[i + 1];
      else if (AttrNameEquals(attr[i], "class"))
        el_class = attr[i + 1];
    }
    if (book_xml_css_style_utils::HasPageBreakAfter(el_style) ||
        epub_css_class_map::LookupPageBreakAfterForClassAttr(
            el_class ? std::string(el_class) : std::string(),
            p->css_class_map)) {
      p->page_break_after_stack[p->stacksize - 1] = true;
    }
  }
}

void chardata(void *data, const XML_Char *txt, int txtlen) {
  //! reflow text on the fly, into page data structure.

  parsedata_t *p = (parsedata_t *)data;
  ChardataPerfScope perf_scope(p);

  if (p->collecting_fb2_binary) {
    if (!p->fb2_binary_too_large) {
      for (int i = 0; i < txtlen; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          continue;
        p->fb2_binary_data.push_back((char)c);
      }
      if (p->fb2_binary_data.size() > kFb2BinaryMaxChars) {
        p->fb2_binary_data.clear();
        p->fb2_binary_too_large = true;
      }
    }
    return;
  }

  if (parse_in(p, TAG_TITLE)) {
    if (p->fb2_mode && p->fb2_title_capture_depth > 0) {
      p->fb2_title_text.append((const char *)txt, txtlen);
    } else {
      p->doc_title.append((const char *)txt, txtlen);
    }
    return;
  }
  if (parse_in(p, TAG_SCRIPT))
    return;
  if (parse_in(p, TAG_STYLE))
    return;
  if (HasActiveStackHiddenStyle(p))
    return;
  if (parse_in(p, TAG_TABLE)) {
    std::string *buffer = GetActiveCapturedTableText(p);
    if (buffer)
      buffer->append((const char *)txt, (size_t)txtlen);
    return;
  }
  if (!p->doc_heading_complete &&
      (parse_in(p, TAG_H1) || parse_in(p, TAG_H2) || parse_in(p, TAG_H3)) &&
      p->doc_heading.size() < 160) {
    p->doc_heading.append((const char *)txt, txtlen);
  }

  if (p->strip_leading_list_marker) {
    bool all_whitespace_only = false;
    size_t strip = text_unicode_utils::StripLeadingListMarkerUtf8(
        txt, (size_t)txtlen, &all_whitespace_only);
    if (strip > 0) {
      txt += strip;
      txtlen -= (int)strip;
      p->strip_leading_list_marker = false;
      if (txtlen <= 0)
        return;
    } else if (all_whitespace_only) {
      return;
    } else {
      p->strip_leading_list_marker = false;
    }
  }

  if (book_xml_list_utils::HasPendingListItemContent(p) &&
      HasVisibleTextContentUtf8(txt, txtlen)) {
    book_xml_list_utils::ConsumePendingListItemContent(p);
  }
  EmitFlowedFragmentRaw(p, txt, txtlen);
}

void end(void *data, const char *el) {
  parsedata_t *p = (parsedata_t *)data;
  Text *ts = p->ts;

  if (XmlNameEquals(el, "binary")) {
    if (p->collecting_fb2_binary && !p->fb2_binary_too_large && p->book &&
        !p->fb2_binary_id.empty() && !p->fb2_binary_data.empty()) {
      p->book->StoreFb2InlineImage(p->fb2_binary_id, p->fb2_binary_data);
    }
    p->collecting_fb2_binary = false;
    p->fb2_binary_too_large = false;
    p->fb2_binary_id.clear();
    p->fb2_binary_data.clear();
    parse_pop(p);
    return;
  }

  if (p->fb2_mode) {
    if (XmlNameEquals(el, "title")) {
      if (p->fb2_title_depth > 0) {
        bool finishing_capture =
            (p->fb2_title_depth == 1 && p->fb2_title_capture_depth > 0 &&
             p->fb2_title_capture_depth == p->fb2_section_depth);
        if (finishing_capture && p->book) {
          std::string chapter_title =
              NormalizeFb2ChapterTitle(p->fb2_title_text);
          if (!chapter_title.empty()) {
            int level = p->fb2_section_depth > 0 ? p->fb2_section_depth - 1 : 0;
            if (level > 255)
              level = 255;
            p->book->AddChapter(p->book->GetPageCount(), chapter_title,
                                (u8)level);
            if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
              p->fb2_section_has_chapter[p->fb2_section_depth] = true;
          }
          p->fb2_title_text.clear();
          p->fb2_title_capture_depth = 0;
        }
        p->fb2_title_depth--;
        if (p->fb2_title_depth < 0)
          p->fb2_title_depth = 0;
      }
    } else if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth > 0) {
        if (p->fb2_section_depth < 32)
          p->fb2_section_has_chapter[p->fb2_section_depth] = false;
        p->fb2_section_depth--;
      }
      if (p->fb2_section_depth < 0)
        p->fb2_section_depth = 0;
      if (p->fb2_title_capture_depth > p->fb2_section_depth) {
        p->fb2_title_capture_depth = 0;
        p->fb2_title_text.clear();
      }
    }
  }

  if (HandleTableEnd(p, ts, el))
    return;

  if (!strcmp(el, "body")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    // Save off our last page.
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    // Retain styles across the page.
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    RestoreParsedInlineLinkMarker(p);
    parse_pop(p);
    return;
  }

  if (!strcmp(el, "br")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    linefeed(p);
  } else if (!strcmp(el, "a")) {
    if (p->stacksize > 0) {
      const u8 current = (u8)(p->stacksize - 1);
      if (p->link_active_stack[current] && p->link_href_id_stack[current] != 0)
        AppendParsedByte(p, TEXT_LINK_END);
    }
    // Many EPUB TOC/Nav documents are built as dense anchor lists with little
    // structural markup; force line breaks there to keep the reading view sane.
    if (DocLooksLikeTocDoc(p) && p->linebegan && p->buflen > 0 &&
        p->buf[p->buflen - 1] != '\n') {
      linefeed(p);
    }
  } else if (!strcmp(el, "aside")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    linefeed(p);
    linefeed(p);
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "blockquote") || !strcmp(el, "caption") ||
             !strcmp(el, "dd") || !strcmp(el, "figure")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    linefeed(p);
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "p")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (p->paragraph_has_content &&
        !book_xml_list_utils::IsInsideListItem(p) &&
        !ParseInAnyEasyParagraphTightBlock(p)) {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          ParseElementMarginBottomWithClass(p->last_p_style, p->last_p_class,
                                            p->css_class_map);
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "p", "bottom", p->last_p_style,
                             p->last_p_class, mbr, line_h, default_lf,
                             lf_count);
      if (lf_count > 0) {
        for (int i = 0; i < lf_count; i++)
          linefeed(p);
      } else if (p->linebegan) {
        linefeed(p);
      }
    }
    RestoreActiveBlockTextAlignMarker(p);
    p->in_paragraph = false;
    p->paragraph_has_content = false;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "div")) {
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "h1")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          ParseElementMarginBottomWithClass(p->last_h1_style, p->last_h1_class,
                                            p->css_class_map);
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h1", "bottom", p->last_h1_style,
                             p->last_h1_class, mbr, line_h, default_lf,
                             lf_count);
      for (int i = 0; i < lf_count; i++)
        linefeed(p);
    }
    RestoreHeadingFontSize(p, ts);
    RestoreActiveBlockTextAlignMarker(p);
    if (!Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "h2")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          ParseElementMarginBottomWithClass(p->last_h2_style, p->last_h2_class,
                                            p->css_class_map);
      const int default_lf = 1;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h2", "bottom", p->last_h2_style,
                             p->last_h2_class, mbr, line_h, default_lf,
                             lf_count);
      for (int i = 0; i < lf_count; i++)
        linefeed(p);
    }
    RestoreHeadingFontSize(p, ts);
    RestoreActiveBlockTextAlignMarker(p);
    if (!Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "h3") || !strcmp(el, "h4") || !strcmp(el, "h5") ||
             !strcmp(el, "h6") || !strcmp(el, "hr")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (!strcmp(el, "hr")) {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          ParseElementMarginBottomWithClass(p->last_hr_style, p->last_hr_class,
                                            p->css_class_map);
      const int default_lf = 2;
      const int lf_count =
          book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
              default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "hr", "bottom", p->last_hr_style,
                             p->last_hr_class, mbr, line_h, default_lf,
                             lf_count);
      if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
        // TEXT_HR was emitted, so the renderer has linebegan=false.  It will
        // never call PrintNewLine() for these \n bytes — only the WouldOverflow
        // path fires to advance screens.  Emit the bytes for that check but do
        // NOT advance pen.y: doing so would diverge from the renderer and
        // cause premature page/screen breaks (Bug: text cut off midline).
        for (int i = 0; i < lf_count; i++)
          AppendParsedByte(p, '\n');
      } else {
        for (int i = 0; i < lf_count; i++)
          linefeed(p);
      }
    } else {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          ParseElementMarginBottomWithClass(p->last_h_style, p->last_h_class,
                                            p->css_class_map);
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, el, "bottom", p->last_h_style,
                             p->last_h_class, mbr, line_h, default_lf,
                             lf_count);
      for (int i = 0; i < lf_count; i++)
        linefeed(p);
      RestoreHeadingFontSize(p, ts);
    }
    if (strcmp(el, "hr"))
      RestoreActiveBlockTextAlignMarker(p);
    if ((!strcmp(el, "h3")) && !Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "pre")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    AppendParsedByte(p, TEXT_PRE_OFF);
    p->preformatted_wrap_enabled = false;
    linefeed(p);
    linefeed(p);
  } else if (!strcmp(el, "code") || !strcmp(el, "tt") ||
             !strcmp(el, "kbd") || !strcmp(el, "samp")) {
  } else if (!strcmp(el, "li") || !strcmp(el, "ul") || !strcmp(el, "ol")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (!strcmp(el, "li"))
      p->strip_leading_list_marker = false;
    linefeed(p);
  }

  bool restore_block_text_align = false;
  bool had_page_break_after = false;
  if (p->stacksize > 0) {
    const u8 current = (u8)(p->stacksize - 1);
    restore_block_text_align = p->block_text_align_stack[current];
    if (IsBlockLevelElement(el))
      had_page_break_after = p->page_break_after_stack[current];
  }

  parse_pop(p);
  if (restore_block_text_align)
    RestoreActiveBlockTextAlignMarker(p);
  if (had_page_break_after)
    ForcePageBreak(p);

  const bool any_reset_bold = HasActiveStackResetBoldStyle(p);
  const bool any_reset_italic = HasActiveStackResetItalicStyle(p);
  const bool any_no_underline = HasActiveStackNoUnderlineStyle(p);
  const bool want_bold =
      !any_reset_bold &&
      (parse_in(p, TAG_STRONG) || parse_in(p, TAG_H1) || parse_in(p, TAG_H2) ||
       parse_in(p, TAG_H3) || parse_in(p, TAG_H4) || parse_in(p, TAG_H5) ||
       parse_in(p, TAG_H6) || HasActiveStackBoldStyle(p));
  const bool want_italic =
      !any_reset_italic && (parse_in(p, TAG_EM) || HasActiveStackItalicStyle(p));
  const bool want_underline =
      !any_no_underline &&
      (parse_in(p, TAG_UNDERLINE) || HasActiveStackUnderlineStyle(p));
  const u8 want_underline_style =
      want_underline ? ResolveActiveUnderlineStyle(p) : UNDERLINE_STYLE_SOLID;
  const bool want_overline = HasActiveStackOverlineStyle(p);
  const bool want_strikethrough = parse_in(p, TAG_STRIKETHROUGH) ||
                                  HasActiveStackStrikethroughStyle(p);
  const bool want_superscript = parse_in(p, TAG_SUPERSCRIPT) ||
                                HasActiveStackSuperscriptStyle(p);
  const bool want_subscript =
      parse_in(p, TAG_SUBSCRIPT) || HasActiveStackSubscriptStyle(p);
  const bool want_mono = parse_in(p, TAG_CODE) || parse_in(p, TAG_PRE) ||
                         HasActiveStackMonoStyle(p);

  const bool needs_style_sync =
      p->bold != want_bold || p->italic != want_italic ||
      p->underline != want_underline ||
      (want_underline && p->underline_style != want_underline_style) ||
      p->overline != want_overline ||
      p->strikethrough != want_strikethrough ||
      p->superscript != want_superscript || p->subscript != want_subscript ||
      p->mono != want_mono;

  if (needs_style_sync) {
    QueueDeferredStyleSync(p, want_bold, want_italic, want_underline,
                           want_underline_style,
                           want_overline, want_strikethrough,
                           want_superscript, want_subscript, want_mono);
    ApplyDeferredStyleSync(p, ts);
  }

  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
          MIN(ts->margin.bottom, 16));
  int maxHeight = metrics.max_height;
  int bottomMargin = metrics.bottom_margin;
  int lineheight = ts->GetHeight();
  if (text_render_layout_utils::WouldOverflowReadingScreen(
          p->pen.y, lineheight, ts->linespacing, maxHeight, bottomMargin)) {
    if (p->screen == 1) {
      // End of right screen; end of page.
      // Copy in buffered char data into a new page.
      Page *page = p->book->AppendPage();
      page->SetBuffer(p->buf, p->buflen);
      parse_reset_page_buffer(p);
      book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
      p->screen = 0;
    } else
      // End of left screen; same page, next screen.
      p->screen = 1;
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + ts->GetHeight();
    p->linebegan = false;
  }
}

int unknown(void *encodingHandlerData, const XML_Char *name,
            XML_Encoding *info) {
  return 0;
}

void fallback(void *data, const XML_Char *s, int len) {
  parsedata_t *p = (parsedata_t *)data;
  if (!p || !s || len <= 0 || s[0] != '&')
    return;

  uint32_t cp = 0;
  if (!html_entity_utils::DecodeHtmlEntityCodepoint(std::string(s, len), &cp))
    return;

  AppendParsedByte(p, (u32)cp);
  p->pen.x += p->ts->GetAdvance(cp);
}

} // namespace xml::book
