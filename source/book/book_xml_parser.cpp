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
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_hidden_utils.h"
#include "book/book_xml_list_utils.h"
#include "book/epub_css_class_map.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_table_utils.h"
#include "book/book_xml_text_emit.h"
#include "book/book_xml.h"
#include "book/inline_image_layout.h"
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

using book_xml_css_style_utils::ResolveHorizontalMarginPx;

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
  parsedata_t *parsedata;
  Text *text;
  u8 style;
  u8 pixel_size;
  int latin1_cache_slot;
};

static void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}

static void ResetLatin1AdvanceCacheSlot(parsedata_t *p, int slot, u8 style,
                                        u8 pixel_size) {
  if (!p)
    return;
  p->latin1_advance_cache_style[slot] = style;
  p->latin1_advance_cache_pixel_size[slot] = pixel_size;
  for (int i = 0; i < 8; i++)
    p->latin1_advance_cache_valid[slot][i] = 0;
}

static int FindLatin1AdvanceCacheSlot(parsedata_t *p, u8 style,
                                      u8 pixel_size) {
  if (!p)
    return -1;

  for (int slot = 0; slot < LATIN1_ADVANCE_CACHE_SLOTS; slot++) {
    if (p->latin1_advance_cache_style[slot] == style &&
        p->latin1_advance_cache_pixel_size[slot] == pixel_size)
      return slot;
  }

  const int slot =
      (int)(p->latin1_advance_cache_next_slot % LATIN1_ADVANCE_CACHE_SLOTS);
  p->latin1_advance_cache_next_slot =
      (u8)((p->latin1_advance_cache_next_slot + 1) %
           LATIN1_ADVANCE_CACHE_SLOTS);
  ResetLatin1AdvanceCacheSlot(p, slot, style, pixel_size);
  return slot;
}

static ParsedTextMeasureContext MakeParsedTextMeasureContext(parsedata_t *p,
                                                             Text *text,
                                                             bool bold,
                                                             bool italic,
                                                             bool mono) {
  ParsedTextMeasureContext ctx{};
  ctx.parsedata = p;
  ctx.text = text;
  ctx.style =
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono);
  ctx.pixel_size = text ? text->GetPixelSize() : 0;
  ctx.latin1_cache_slot =
      text ? FindLatin1AdvanceCacheSlot(p, ctx.style, ctx.pixel_size) : -1;
  return ctx;
}

static int MeasureParsedTextAdvance(uint32_t codepoint, void *ctx) {
  ParsedTextMeasureContext *measure = (ParsedTextMeasureContext *)ctx;
  if (!measure || !measure->text)
    return 0;
  if (codepoint < 256 && measure->parsedata &&
      measure->latin1_cache_slot >= 0) {
    parsedata_t *p = measure->parsedata;
    const int slot = measure->latin1_cache_slot;

    const int word = (int)(codepoint >> 5);
    const u32 mask = (u32)1 << (codepoint & 31);
    if (p->latin1_advance_cache_valid[slot][word] & mask)
      return p->latin1_advance_cache[slot][codepoint];

    const int advance = measure->text->GetAdvance(codepoint, measure->style);
    p->latin1_advance_cache[slot][codepoint] = (u8)advance;
    p->latin1_advance_cache_valid[slot][word] |= mask;
    return advance;
  }
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
  book_xml_inline_state::QueueDeferredStyleSync(
      p, want_bold, want_italic, want_underline, want_underline_style,
      want_overline, want_strikethrough, want_superscript, want_subscript,
      want_mono);
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
ParseElementMarginTopPx(const char **attr) {
  return book_xml_css_resolver::ParseElementMarginTopPx(attr);
}

// Return the author-specified max width in pixels for an img element.
// Checks the HTML width attribute first, then the CSS width property in style.
// Returns 0 if no usable constraint found.
static int ParseImgWidthPx(const char *width_attr, const char *style,
                            int text_width, int font_px) {
  return book_xml_css_resolver::ParseImgWidthPx(width_attr, style, text_width,
                                                 font_px);
}

static std::string ExtractStyleAttr(const char **attr) {
  return book_xml_css_resolver::ExtractStyleAttr(attr);
}

static std::string ExtractClassAttr(const char **attr) {
  return book_xml_css_resolver::ExtractClassAttr(attr);
}



static void AlignFreshLineToBlockMargin(parsedata_t *p, Text *ts) {
  if (!p || !ts)
    return;
  const int x = std::max(0, ts->margin.left + p->block_margin_left);
  if (p->pen.x == x)
    return;
  p->pen.x = x;
  if (!p->linebegan) {
    AppendParsedByte(p, TEXT_LINE_START_X);
    AppendParsedByte(p, (u32)x);
  }
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
ParseElementMarginBottomWithClass(const std::string &last_style,
                                  const std::string &last_class,
                                  const epub_css_class_map::CssClassMap &class_map) {
  return book_xml_css_resolver::ParseElementMarginBottomWithClass(last_style, last_class, class_map);
}

static book_xml_css_style_utils::TextAlign
ResolveElementTextAlignWithClass(const std::string &style_attr,
                                 const std::string &class_attr,
                                 const parsedata_t *p,
                                 const epub_css_class_map::CssClassMap &class_map) {
  if (!p)
    return book_xml_css_style_utils::TextAlign::Left;
  return book_xml_css_resolver::ResolveElementTextAlignWithClass(
      style_attr, class_attr, p->block_text_align_stack,
      p->block_text_align_value_stack, p->stacksize, class_map);
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

static bool ElementCanCarryBlockTextAlign(const char *el,
                                          const std::string &style_attr) {
  return book_xml_css_resolver::ElementCanCarryBlockTextAlign(el, style_attr);
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
  return book_xml_css_resolver::ParseElementStyleFlags(
      attr, bold_out, italic_out, underline_out,
      reinterpret_cast<uint8_t *>(underline_style_out),
      overline_out, strikethrough_out, superscript_out, subscript_out,
      no_underline_out, reset_bold_out, reset_italic_out);
}

static void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  return book_xml_css_resolver::ParseElementHiddenFlags(attr, hidden_out);
}

static bool HasActiveStackBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackBoldStyle(p);
}
static bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackHiddenStyle(p);
}
static bool HasActiveStackItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackItalicStyle(p);
}
static bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackUnderlineStyle(p);
}
static u8 ResolveActiveUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::ResolveActiveUnderlineStyle(p);
}
static bool HasActiveStackOverlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackOverlineStyle(p);
}
static bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackStrikethroughStyle(p);
}
static bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSuperscriptStyle(p);
}
static bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSubscriptStyle(p);
}
static bool HasActiveStackNoUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackNoUnderlineStyle(p);
}
static bool HasActiveStackResetBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetBoldStyle(p);
}
static bool HasActiveStackResetItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetItalicStyle(p);
}
static bool HasActiveStackMonoStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackMonoStyle(p);
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

struct ElementPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit ElementPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~ElementPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_element_calls++;
    parsedata->perf_element_ms += (u64)(osGetTime() - t_begin);
  }
};

struct FlushPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit FlushPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~FlushPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_flush_calls++;
    parsedata->perf_flush_ms += (u64)(osGetTime() - t_begin);
  }
};
#else
struct ChardataPerfScope {
  explicit ChardataPerfScope(parsedata_t *) {}
};
struct ElementPerfScope {
  explicit ElementPerfScope(parsedata_t *) {}
};
struct FlushPerfScope {
  explicit FlushPerfScope(parsedata_t *) {}
};
#endif

static void EmitFlowedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx,
    const book_xml_text_emit::FlowEmitMetrics &emit_metrics) {
  if (!p || !txt || txtlen == 0)
    return;

  std::vector<text_layout_utils::ShapedGlyph> &run = p->shaped_run;
  bool has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &run, &has_rtl, &p->bidi_cps, &p->bidi_runs))
    return;

  book_xml_text_emit::EmitFlowedShapedText(
      p, txt, run, has_rtl, p->bidi_runs, emit_metrics,
      AdvanceParsedPageOnOverflowThunk, NULL);
}

static void EmitPreformattedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx, int lineheight,
    int linespacing, bool allow_wrap, bool text_already_transformed) {
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
      if (text_already_transformed)
        book_xml_text_emit::AppendParsedCodepointsRaw(p, txt + offset, step);
      else
        book_xml_text_emit::AppendParsedCodepoints(p, txt + offset, step);
      p->pen.x += MeasureParsedTextAdvance(cp, (void *)&measure_ctx);
      p->linebegan = true;
      offset += step;
    }
    return;
  }

  std::vector<text_layout_utils::ShapedGlyph> &pre_run = p->shaped_run;
  bool pre_has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &pre_run, &pre_has_rtl, &p->bidi_cps, &p->bidi_runs)) {
    return;
  }

  if (pre_has_rtl) {
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
                                          segment_end_index, p->bidi_runs,
                                          !text_already_transformed);
    } else {
      if (text_already_transformed) {
        book_xml_text_emit::AppendParsedCodepointsRaw(
            p, txt + segment_start, segment_end - segment_start);
      } else {
        book_xml_text_emit::AppendParsedCodepoints(
            p, txt + segment_start, segment_end - segment_start);
      }
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
  const ParsedTextMeasureContext measure_ctx =
      MakeParsedTextMeasureContext(p, ts, p->bold, p->italic, p->mono);

  int lineheight = ts->GetHeight();
  int linespacing = ts->linespacing;
  int spaceadvance = MeasureParsedTextAdvance((u16)' ', (void *)&measure_ctx);

  if (p->buflen == 0) {
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + lineheight;
    p->linebegan = false;
  }

  std::string transformed_text;
  const char *flow_txt = txt;
  size_t flow_txtlen = (size_t)txtlen;
  bool text_already_transformed = false;
  if (parse_resolve_text_transform(p) != 0) {
    transformed_text =
        book_xml_text_emit::TransformUtf8ForLayout(p, txt, (size_t)txtlen);
    flow_txt = transformed_text.c_str();
    flow_txtlen = transformed_text.size();
    text_already_transformed = true;
  }

  const book_xml_css_style_utils::WhiteSpaceMode white_space =
      ResolveActiveWhiteSpace(p);
  book_xml_text_emit::FlowEmitMetrics emit_metrics{};
  emit_metrics.display_width = ts->display.width;
  emit_metrics.base_margin_left = ts->margin.left;
  emit_metrics.margin_left = ts->margin.left + p->block_margin_left;
  emit_metrics.margin_right = ts->margin.right + p->block_margin_right;
  emit_metrics.lineheight = lineheight;
  emit_metrics.linespacing = linespacing;
  emit_metrics.spaceadvance = spaceadvance;
  emit_metrics.text_already_transformed = text_already_transformed;

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap) {
    EmitPreformattedUtf8Segment(
        p, flow_txt, flow_txtlen, measure_ctx, lineheight, linespacing,
        white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap,
        text_already_transformed);
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Nowrap) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            flow_txt, flow_txtlen, white_space);
    if (!normalized.empty())
      EmitFlowedUtf8Segment(p, normalized.c_str(), normalized.size(),
                            measure_ctx, emit_metrics);
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::PreLine) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            flow_txt, flow_txtlen, white_space);
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

  EmitFlowedUtf8Segment(p, flow_txt, flow_txtlen, measure_ctx, emit_metrics);
}

static void FlushInlineTextTail(parsedata_t *p) {
  if (!p || p->inline_text_tail.empty())
    return;
  FlushPerfScope perf_scope(p);
  std::string tail;
  tail.swap(p->inline_text_tail);
  EmitFlowedFragmentRaw(p, tail.c_str(), (int)tail.size());
}

static void QueueFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt,
                                   int txtlen) {
  if (!p || !txt || txtlen <= 0)
    return;
  if (!p->coalesce_text_segments) {
    EmitFlowedFragmentRaw(p, txt, txtlen);
    return;
  }

  static const size_t kInlineTextTailFlushBytes = 4096;
  const size_t len = (size_t)txtlen;
  if (!p->inline_text_tail.empty() &&
      p->inline_text_tail.size() + len > kInlineTextTailFlushBytes)
    FlushInlineTextTail(p);
  if (len > kInlineTextTailFlushBytes) {
    EmitFlowedFragmentRaw(p, txt, txtlen);
    return;
  }
  p->inline_text_tail.append(txt, len);
}

static void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts) {
  FlushInlineTextTail(p);
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

static void ApplyClearBreak(parsedata_t *p) {
  if (!p || !p->linebegan || blankline(p))
    return;
  linefeed(p);
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

static void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
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
  // If nothing is buffered yet we are already at the top of a fresh screen.
  if (p->buflen == 0)
    return;
  if (p->screen == 0) {
    // Emit a marker so page.cpp knows to switch to screen=1 at this position.
    AppendParsedByte(p, TEXT_SCREEN_BREAK);
  }
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

// True if element is a native block element OR promoted to block via CSS.
static bool BehavesAsBlock(const char *el,
                           const epub_css_class_map::CssClassMargins &elem_css) {
  return IsBlockLevelElement(el) || elem_css.is_display_block;
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

// Pre-resolved CSS overloads — accept a single LookupAllForClassAttr result
// instead of re-scanning the class map for each property.

static book_xml_css_style_utils::ClearMode
ParseElementClear(const char **attr,
                  const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::ClearMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        ClearMode mode = ClearMode::None;
        if (book_xml_css_style_utils::TryParseClear(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_clear ? elem_css.clear_mode : ClearMode::None;
}

static book_xml_css_style_utils::FloatMode
ParseElementFloat(const char **attr,
                  const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::FloatMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        FloatMode mode = FloatMode::None;
        if (book_xml_css_style_utils::TryParseFloat(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_float ? elem_css.float_mode : FloatMode::None;
}

static u8 ParseElementTextTransform(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::TextTransform;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const TextTransform tt =
            book_xml_css_style_utils::ParseTextTransform(attr[i + 1]);
        if (tt != TextTransform::None)
          return (u8)tt;
      }
    }
  }
  return elem_css.has_text_transform ? (u8)elem_css.text_transform : 0;
}

static u8 ParseElementWhiteSpace(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::WhiteSpaceMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        WhiteSpaceMode mode = WhiteSpaceMode::Normal;
        if (book_xml_css_style_utils::TryParseWhiteSpace(attr[i + 1], &mode))
          return (u8)mode + 1;
      }
    }
  }
  return elem_css.has_white_space ? (u8)elem_css.white_space + 1 : 0;
}

static void ApplyElementBlockMargins(
    parsedata_t *p, Text *ts, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !ts)
    return;
  using book_xml_css_style_utils::MarginTopResult;
  const int inherited_left = parse_current_block_margin_left(p);
  const int inherited_right = parse_current_block_margin_right(p);
  int effective_left = inherited_left;
  int effective_right = inherited_right;

  MarginTopResult ml, mr;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEquals(attr[i], "style")) {
        const auto r = book_xml_css_style_utils::ParseMarginLeft(attr[i + 1]);
        if (r.unit != MarginTopResult::Unit::None)
          ml = r;
        const auto r2 = book_xml_css_style_utils::ParseMarginRight(attr[i + 1]);
        if (r2.unit != MarginTopResult::Unit::None)
          mr = r2;
        break;
      }
    }
  }
  if (ml.unit == MarginTopResult::Unit::None &&
      elem_css.margin_left.unit != MarginTopResult::Unit::None)
    ml = elem_css.margin_left;
  if (mr.unit == MarginTopResult::Unit::None &&
      elem_css.margin_right.unit != MarginTopResult::Unit::None)
    mr = elem_css.margin_right;

  if (ml.unit != MarginTopResult::Unit::None)
    effective_left += ResolveHorizontalMarginPx(ml, ts->display.width);
  if (mr.unit != MarginTopResult::Unit::None)
    effective_right += ResolveHorizontalMarginPx(mr, ts->display.width);
  parse_set_current_block_margins(p, effective_left, effective_right);
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopWithClass(const char **attr,
                               const epub_css_class_map::CssClassMargins &elem_css) {
  const book_xml_css_style_utils::MarginTopResult from_style =
      ParseElementMarginTopPx(attr);
  if (from_style.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
    return from_style;
  return elem_css.margin_top;
}

static book_xml_css_style_utils::MarginTopResult
ParseElementTextIndentWithClass(const char **attr,
                                const epub_css_class_map::CssClassMargins &elem_css) {
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
  return elem_css.text_indent;
}

static void ConfigureBlockTextAlign(
    parsedata_t *p, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !el || p->stacksize == 0)
    return;
  const std::string style_attr = ExtractStyleAttr(attr);
  const bool can_carry = ElementCanCarryBlockTextAlign(el, style_attr) ||
                         elem_css.is_display_block;
  if (!can_carry)
    return;

  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  bool has_align =
      book_xml_css_style_utils::TryParseTextAlign(style_attr.c_str(), &align);
  if (!has_align && elem_css.has_text_align) {
    has_align = true;
    align = elem_css.text_align;
  }
  if (!has_align)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  p->block_text_align_stack[current] = true;
  p->block_text_align_value_stack[current] = (u8)align;
  AppendParagraphAlignMarker(p, align);
}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  ElementPerfScope elem_perf(p);
  Text *ts = p->ts;
  FlushInlineTailAndDeferredStyle(p, ts);

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

  // One-shot CSS class resolution for this element — avoids 18+ separate
  // map lookups by resolving all CSS properties in a single pass.
  const char *el_class_raw = nullptr;
  const char *el_style_raw = nullptr;
  for (int i = 0; attr && attr[i]; i += 2) {
    if (AttrNameEquals(attr[i], "class"))
      el_class_raw = attr[i + 1];
    else if (AttrNameEquals(attr[i], "style"))
      el_style_raw = attr[i + 1];
  }
  epub_css_class_map::CssClassMargins elem_css;
  epub_css_class_map::LookupAllForClassAttr(
      el_class_raw ? std::string(el_class_raw) : std::string(),
      p->css_class_map, &elem_css);

  if (BehavesAsBlock(el, elem_css)) {
    if (ParseElementClear(attr, elem_css) !=
        book_xml_css_style_utils::ClearMode::None) {
      ApplyClearBreak(p);
    }
    if ((book_xml_css_style_utils::HasPageBreakInsideAvoid(el_style_raw) ||
         elem_css.page_break_inside_avoid) &&
        p->buflen > 0 && !blankline(p)) {
      ForcePageBreak(p);
    }
    if (book_xml_css_style_utils::HasPageBreakBefore(el_style_raw) ||
        elem_css.page_break_before) {
      ForcePageBreak(p);
    }
  }

  if (!strcmp(el, "html"))
    parse_push(p, TAG_HTML);
  else if (!strcmp(el, "aside")) {
    parse_push(p, TAG_ASIDE);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "blockquote")) {
    parse_push(p, TAG_BLOCKQUOTE);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    if (!blankline(p))
      linefeed(p);
  } else if (!strcmp(el, "dd")) {
    parse_push(p, TAG_DD);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
    ApplyElementBlockMargins(p, ts, attr, elem_css);
  }
  else if (!strcmp(el, "dt")) {
    parse_push(p, TAG_DT);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
  }
  else if (!strcmp(el, "figure")) {
    parse_push(p, TAG_FIGURE);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
        ParseElementMarginTopWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
        ParseElementMarginTopWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
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
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
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
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
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
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
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
  else if (!strcmp(el, "ol")) {
    parse_push(p, TAG_OL);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  } else if (!strcmp(el, "p")) {
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
        ParseElementMarginTopWithClass(attr, elem_css);
    const book_xml_css_style_utils::MarginTopResult text_indent_mtr =
        ParseElementTextIndentWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
        ParseElementMarginTopWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
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
    const int nested_indent = book_xml_list_utils::ResolveNestedListItemIndentPx(
        book_xml_list_utils::GetActiveListDepth(p), ts->GetAdvance(' '));
    if (nested_indent != 0) {
      parse_set_current_block_margins(
          p, parse_current_block_margin_left(p) + nested_indent,
          parse_current_block_margin_right(p));
    }
    // HasSuppressedListMarkerContext checks ancestor elements (e.g. ol.classname).
    // ParseListMarkerHiddenCssClass checks the <li> element's own class
    // attribute, which ConfigureElementListSemantics hasn't processed yet.
    const bool suppress_marker =
        book_xml_list_utils::HasSuppressedListMarkerContext(p) ||
        book_xml_list_utils::ParseListMarkerHiddenCssClass(p, attr);
    if (active_list == TAG_UL || active_list == TAG_OL) {
      if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
        linefeed(p);
      // Prevent orphan markers: if the marker's line is the last usable line on
      // the screen (chardata would immediately advance before the first content
      // character), push the marker to the next screen now.
      AdvanceParsedPageOnOverflow(p, ts->GetHeight());
      AlignFreshLineToBlockMargin(p, ts);
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
  else if (!strcmp(el, "ul")) {
    parse_push(p, TAG_UL);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  } else if (!strcmp(el, "strong") || !strcmp(el, "b")) {
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

    const book_xml_css_style_utils::FloatMode float_mode =
        ParseElementFloat(attr, elem_css);
    if (ParseElementClear(attr, elem_css) !=
        book_xml_css_style_utils::ClearMode::None) {
      ApplyClearBreak(p);
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

      if (float_mode != book_xml_css_style_utils::FloatMode::None)
        ApplyFloatImageLayoutOverride(&image_plan, p->linebegan,
                                      ts->linespacing);

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
      if (author_max_w > 0) {
        AppendParsedByte(p, TEXT_IMAGE_AUTHOR_WIDTH);
        AppendParsedByte(p, (u32)author_max_w);
      }
      if (float_mode == book_xml_css_style_utils::FloatMode::Left) {
        AppendParsedByte(p, TEXT_IMAGE_ALIGN);
        AppendParsedByte(p, 1);
      } else if (float_mode == book_xml_css_style_utils::FloatMode::Right) {
        AppendParsedByte(p, TEXT_IMAGE_ALIGN);
        AppendParsedByte(p, 2);
      }
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

  ConfigureBlockTextAlign(p, el, attr, elem_css);

  // Promote inline elements with CSS display:block to block layout:
  // margins, text-indent, and line breaks. Only for non-native-block elements
  // that are NOT inside a paragraph or other inline context.
  if (!IsBlockLevelElement(el) && !p->in_paragraph &&
      elem_css.is_display_block && ts) {
    if (!blankline(p))
      linefeed(p);
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    const int line_h = ts->GetHeight() + ts->linespacing;
    const int default_lf = 1;
    const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
        default_lf, mtr, line_h);
    EmitAdditionalTopLinefeeds(p, lf_count);
    const book_xml_css_style_utils::MarginTopResult text_indent_mtr =
        ParseElementTextIndentWithClass(attr, elem_css);
    using book_xml_css_style_utils::MarginTopResult;
    if (text_indent_mtr.unit != MarginTopResult::Unit::None &&
        !text_indent_mtr.negative) {
      const int sa = ts->GetAdvance(' ');
      if (sa > 0) {
        const int indent_spaces = text_indent_mtr.value / sa;
        for (int i = 0; i < indent_spaces; i++) {
          AppendParsedByte(p, ' ');
          p->pen.x += sa;
        }
      }
    }
  }

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
    // Use pre-resolved CSS class properties from elem_css.
    if (!style_superscript) style_superscript = elem_css.superscript;
    if (!style_subscript) style_subscript = elem_css.subscript;
    if (!style_no_underline) style_no_underline = elem_css.no_underline;
    if (!style_reset_bold) style_reset_bold = elem_css.reset_bold;
    if (!style_reset_italic) style_reset_italic = elem_css.reset_italic;
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
        ParseElementTextTransform(attr, elem_css);
    p->style_white_space_stack[current] =
        ParseElementWhiteSpace(attr, elem_css);

    // Font-size: <small>/<big> and CSS font-size (headings manage their own)
    {
      const bool is_heading_el = (el[0] == 'h' && el[1] >= '1' && el[1] <= '6' && !el[2]);
      u8 new_font_px = 0;
      if (!is_heading_el) {
        book_xml_css_style_utils::FontSizeSpec spec;
        bool has_spec = book_xml_css_style_utils::TryParseFontSize(
            ExtractStyleAttr(attr).c_str(), &spec);
        if (!has_spec && elem_css.font_size.unit != book_xml_css_style_utils::FontSizeSpec::Unit::None) {
          spec = elem_css.font_size;
          has_spec = true;
        }
        if (!has_spec) {
          if (!strcmp(el, "small")) {
            spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Smaller;
            has_spec = true;
          } else if (!strcmp(el, "big")) {
            spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Larger;
            has_spec = true;
          }
        }
        if (has_spec &&
            spec.unit != book_xml_css_style_utils::FontSizeSpec::Unit::None) {
          if (p->base_font_size_px == 0)
            p->base_font_size_px = ts->GetPixelSize();
          const int px = book_xml_css_style_utils::ResolveFontSizePx(
              spec, (int)ts->GetPixelSize());
          new_font_px = (u8)book_xml_parser_style_utils::ClampInlineFontSize(
              p->base_font_size_px, px);
          if (new_font_px == ts->GetPixelSize())
            new_font_px = 0;
        }
      }
      if (new_font_px) {
        p->style_font_size_stack[current] = new_font_px;
        p->style_font_size_restore_stack[current] = ts->GetPixelSize();
        ts->SetPixelSize(new_font_px);
        AppendParsedByte(p, TEXT_FONT_SIZE);
        AppendParsedByte(p, new_font_px);
      } else {
        p->style_font_size_stack[current] = 0;
        p->style_font_size_restore_stack[current] = 0;
      }
    }

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

  if (BehavesAsBlock(el, elem_css) && p->stacksize > 0) {
    if (book_xml_css_style_utils::HasPageBreakAfter(el_style_raw) ||
        elem_css.page_break_after) {
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
  QueueFlowedFragmentRaw(p, txt, txtlen);
}

void end(void *data, const char *el) {
  parsedata_t *p = (parsedata_t *)data;
  ElementPerfScope elem_perf(p);
  Text *ts = p->ts;
  FlushInlineTailAndDeferredStyle(p, ts);

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
    if (p->linebegan)
      linefeed(p);
  } else if (!IsBlockLevelElement(el) && p->stacksize > 0 &&
             p->block_text_align_stack[(u8)(p->stacksize - 1)]) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (p->linebegan)
      linefeed(p);
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  }

  bool restore_block_text_align = false;
  bool had_page_break_after = false;
  u8 restore_font_size_px = 0;
  if (p->stacksize > 0) {
    const u8 current = (u8)(p->stacksize - 1);
    restore_block_text_align = p->block_text_align_stack[current];
    if (IsBlockLevelElement(el) || p->page_break_after_stack[current])
      had_page_break_after = p->page_break_after_stack[current];
    restore_font_size_px = p->style_font_size_restore_stack[current];
  }

  parse_pop(p);
  if (restore_block_text_align)
    RestoreActiveBlockTextAlignMarker(p);
  if (had_page_break_after)
    ForcePageBreak(p);
  if (restore_font_size_px) {
    ts->SetPixelSize(restore_font_size_px);
    AppendParsedByte(p, TEXT_FONT_SIZE);
    AppendParsedByte(p, restore_font_size_px);
  }

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

  FlushInlineTailAndDeferredStyle(p, p->ts);
  AppendParsedByte(p, (u32)cp);
  p->pen.x += p->ts->GetAdvance(cp);
}

} // namespace xml::book
