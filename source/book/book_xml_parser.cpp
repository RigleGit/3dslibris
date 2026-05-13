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
#include "book/book_xml_table_handler.h"
#include "book/book_xml_heading_handler.h"
#include "book/book_xml_image_handler.h"
#include "book/book_xml_anchor_handler.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_text_emit.h"
#include "book/book_xml.h"
#include "book/inline_image_layout.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/html_entity_utils.h"
#include "reader/inline_link_utils.h"
#include "book/heading_layout.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "shared/main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "shared/debug_log.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "settings/prefs.h"
#include "ui/screen_constants.h"
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
constexpr int kCompactBottomMargin = 20;

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

static bool ClassListContains(const char *class_attr, const char *needle) {
  if (!class_attr || !needle || !needle[0])
    return false;

  const std::string classes = ToLowerAsciiLocal(class_attr);
  const std::string target = ToLowerAsciiLocal(needle);
  size_t pos = 0;
  while (pos < classes.size()) {
    while (pos < classes.size() &&
           (classes[pos] == ' ' || classes[pos] == '\t' ||
            classes[pos] == '\r' || classes[pos] == '\n'))
      pos++;
    const size_t start = pos;
    while (pos < classes.size() &&
           classes[pos] != ' ' && classes[pos] != '\t' &&
           classes[pos] != '\r' && classes[pos] != '\n')
      pos++;
    if (pos > start && classes.substr(start, pos - start) == target)
      return true;
  }
  return false;
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
                                  const epub_css_class_map::CssClassMap &class_map,
                                  const char *element_tag = nullptr) {
  return book_xml_css_resolver::ParseElementMarginBottomWithClass(
      last_style, last_class, class_map, element_tag);
}

static book_xml_css_style_utils::TextAlign
ResolveElementTextAlignWithClass(const std::string &style_attr,
                                 const std::string &class_attr,
                                 const parsedata_t *p,
                                 const epub_css_class_map::CssClassMap &class_map,
                                 const char *element_tag = nullptr) {
  if (!p)
    return book_xml_css_style_utils::TextAlign::Left;
  return book_xml_css_resolver::ResolveElementTextAlignWithClass(
      style_attr, class_attr, p->block_text_align_stack,
      p->block_text_align_value_stack, p->stacksize, class_map, element_tag);
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

static bool ImagePathLooksLikeSvgWrapper(const std::string &path) {
  static const std::vector<u8> empty;
  return epub_image_utils::LooksLikeSvgWrapper(path, empty);
}

static void LogResolvedBlockMargin(parsedata_t *p, const char *tag,
                                   const char *phase,
                                   const std::string &style_attr,
                                   const std::string &class_attr,
                                   const book_xml_css_style_utils::MarginTopResult &m,
                                   int line_h, int default_lf, int final_lf) {
  (void)p; (void)tag; (void)phase; (void)style_attr;
  (void)class_attr; (void)m; (void)line_h; (void)default_lf; (void)final_lf;
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

// Forward declaration — defined below after ClearPendingBlockSpacing.
static void ClearPendingBlockSpacing(parsedata_t *p);


static void AppendScreenBreakIfNeeded(parsedata_t *p) {
  if (!p)
    return;

  if (p->buflen <= 0)
    return;

  if (p->buf[p->buflen - 1] == TEXT_SCREEN_BREAK)
    return;

  AppendParsedByte(p, TEXT_SCREEN_BREAK);
}

static void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight) {
  if (!p || !p->book || !p->ts)
    return;

  Text *ts = p->ts;
  const int leftBottomMargin = ts->margin.bottom;
  const int rightBottomMargin = text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom);
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
    AppendScreenBreakIfNeeded(p);
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + lineheight;
  p->current_screen_has_drawable_content = false;
  // Pending spacing accumulated before the overflow is no longer relevant;
  // we are at the top of a fresh screen/page.
  ClearPendingBlockSpacing(p);
}

// ---------------------------------------------------------------------------
// Pending block-layout spacing model helpers.
// See parsedata_t::pending_block_spacing_lf for full doc.
// ---------------------------------------------------------------------------

// Forward declaration — linefeed_r is defined later in this file.
static void linefeed_r(parsedata_t *p, const char *tag, const char *reason,
                       int is_real);

// Forward declaration — AdvanceParsedScreen is defined later in this file.
static void AdvanceParsedScreen(parsedata_t *p);

// Forward declaration — AppendScreenBreakIfNeeded is defined later in this file.
static void AppendScreenBreakIfNeeded(parsedata_t *p);

// Returns true only when the current screen has no drawable content since
// the last screen/page advance.  This is the correct way to determine that
// we are at the top of a visually empty screen.
//
// IMPORTANT: pen.y == top_y is NOT sufficient.  After drawing the first text
// line, pen.y remains at top_y until the next real linefeed.  Checking only
// pen.y would incorrectly discard mandatory block separators for elements
// that follow a heading or paragraph drawn on the first line.
static bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p) {
  return p && !p->current_screen_has_drawable_content;
}

static void ClearPendingBlockSpacing(parsedata_t *p) {
  if (!p)
    return;
  p->pending_block_break = false;
  p->pending_block_spacing_lf = 0;
  p->pending_block_spacing_reason = NULL;
  p->pending_block_spacing_from_css = false;
}

// Queue layout-only block spacing.
//
// lines = total requested lines (1 mandatory break + (lines-1) optional).
// Mandatory break:  pending_block_break = true (always when lines >= 1).
// Optional spacing: pending_block_spacing_lf = max(current, lines-1).  Max-collapse.
// Pass from_css=true when the spacing comes from an explicit CSS property.
static void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                                   const char *reason, bool from_css) {
  if (!p || lines <= 0)
    return;
  p->pending_block_break = true;   // mandatory: must start new line before content
  const int new_opt = lines - 1;   // optional lines beyond the mandatory break
  const int prev_opt = p->pending_block_spacing_lf;
  const int next_opt = (new_opt > prev_opt) ? new_opt : prev_opt; // max-collapse
  p->pending_block_spacing_lf = next_opt;
  if (reason)
    p->pending_block_spacing_reason = reason;
  if (from_css)
    p->pending_block_spacing_from_css = true;
  (void)tag;
}

// Suppress ONLY the optional spacing.
// The mandatory block_break is preserved — CSS margin:0 or negative means
// "no extra space", not "collapse onto same line as previous content".
static void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                               const char *reason) {
  if (!p)
    return;
  (void)tag; (void)reason;
  p->pending_block_spacing_lf = 0;          // clear optional spacing
  p->pending_block_spacing_reason = "css-suppress";
  p->pending_block_spacing_from_css = true;
  // pending_block_break is intentionally NOT cleared.
}

// Queue spacing from a CSS margin result.
// Positive margin → max-collapse optional spacing (QueueBlockSpacingLines).
// Zero or negative → mandatory break preserved, optional spacing suppressed.
// No CSS (Unit::None) → use default_lf.
static void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  if (mtr.unit == Unit::None) {
    // No explicit CSS: use default, max-collapse.
    QueueBlockSpacingLines(p, default_lf, tag, reason, false);
    return;
  }
  if (mtr.negative) {
    // CSS negative margin: mandatory break only, user preference overrides.
    QueueBlockSpacingLines(p, default_lf, tag, reason, false);
    return;
  }
  if (mtr.value == 0) {
    // CSS margin:0 — publisher wants no extra gap. Still respect user's
    // paragraph spacing preference (default_lf) as the minimum so that
    // books using margin:0 don't override the user's spacing setting.
    QueueBlockSpacingLines(p, default_lf, tag, reason, false);
    return;
  }
  // Explicit CSS positive: resolve to lines, max-collapse optional spacing.
  const int css_lf = (line_h > 0) ? ((mtr.value + line_h - 1) / line_h) : 0;
  const int resolved = (css_lf > default_lf) ? css_lf : default_lf;
  const int clamped = book_xml_parser_style_utils::ClampResolvedBlockLinefeeds(resolved);
  QueueBlockSpacingLines(p, clamped, tag, reason, true);
}

// Flush pending block spacing just before real content is emitted.
//
// Content-aware rule (NOT equivalent to min(pending, available)):
//   available = floor((max_h - bot_margin - pen_y) / line_step)
//   required  = 1   (1 slot reserved for the next content line)
//
//   Case A: pending + required <= available  →  emit all pending
//   Case B: required <= available            →  emit available - required
//                                               (discard excess to preserve 1 slot)
//   Case C: required > available             →  emit 0, content will force
//                                               AdvanceParsedPageOnOverflow
//
// Known limitation (heading orphan edge-case):
//   When avail == total_heading_lines (e.g. 2 for h2/h3) and pending > 0,
//   flush emits avail-1 = 1 line, leaving only 1 slot.  If the heading needs
//   2 slots (itself + content), it overflows as an orphan.  This requires
//   passing required = total_heading_lines from the heading context to fix.
//   The keep-with-next logic in HandleHeadingStart uses pending-offset to
//   approximate this and advances pre-emptively in most such cases.
//
// Pending is cleared regardless of how much was emitted.
static void FlushPendingBlockSpacingBeforeContent(parsedata_t *p,
                                                  const char *next_tag) {
  if (!p || !p->ts || !p->book)
    return;
  if (!p->pending_block_break && p->pending_block_spacing_lf <= 0) {
    ClearPendingBlockSpacing(p);
    return;
  }

  Text *ts = p->ts;
  const int lh = ts->GetHeight();
  const int ls = ts->linespacing;
  const int line_step = lh + (ls > 0 ? ls : 0);
  if (line_step <= 0) {
    ClearPendingBlockSpacing(p);
    return;
  }

  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
  int available = 0;
  {
    const int usable = metrics.max_height - metrics.bottom_margin - p->pen.y;
    available = (usable > 0) ? (usable / line_step) : 0;
  }

  // ---- Phase 1: mandatory block break -----------------------------------
  // A mandatory break is needed only when linebegan==true (content drawn on
  // current line). If the line is fresh (linebegan==false) we are already at
  // the start of a new line and no extra linefeed is required.
if (p->pending_block_break && p->linebegan) {
  if (available >= 1) {
    linefeed_r(p, next_tag ? next_tag : "?", "mandatory-break", 0);
    available--;   // pen.y advanced one line_step
  } else {
    // We still owe a mandatory block break, but there is no vertical room
    // left on this screen. Do not let the next block continue on the same
    // visual line; move to the next screen/page before emitting content.
    AdvanceParsedScreen(p);

    const text_render_layout_utils::ReadingScreenMetrics after_metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0,
            p->screen,
            ts->margin.bottom,
            text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));

    const int usable_after =
        after_metrics.max_height - after_metrics.bottom_margin - p->pen.y;

    available = (usable_after > 0) ? (usable_after / line_step) : 0;
    }
  }
  p->pending_block_break = false;

  // ---- Phase 2: optional spacing ----------------------------------------
  const int opt = p->pending_block_spacing_lf;
  int emit_opt = 0;
  if (opt > 0 && !IsCurrentReadingScreenVisuallyEmpty(p)) {
    const int required = 1;
    if (opt + required <= available) {
      emit_opt = opt;
    } else if (required <= available) {
      emit_opt = available - required;
      if (emit_opt < 0) emit_opt = 0;
    }
    for (int i = 0; i < emit_opt; i++)
      linefeed_r(p, next_tag ? next_tag : "?", "pending-spacing", 0);
  }

  ClearPendingBlockSpacing(p);
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

#else
struct ChardataPerfScope {
  explicit ChardataPerfScope(parsedata_t *) {}
};
struct ElementPerfScope {
  explicit ElementPerfScope(parsedata_t *) {}
};
#endif

// Adapters bridging anonymous-namespace functions into the FlowEmissionFns
// callback struct used by book_xml_flow_emission.cpp.
static void FlowAdvanceScreen(parsedata_t *p) { AdvanceParsedScreen(p); }
static void FlowAdvancePageOverflow(parsedata_t *p, int lh) {
  AdvanceParsedPageOnOverflow(p, lh);
}
static void FlowFlushPendingBlock(parsedata_t *p, const char *tag) {
  FlushPendingBlockSpacingBeforeContent(p, tag);
}
static FlowEmissionFns MakeFlowEmissionFns() {
  FlowEmissionFns f;
  f.advance_screen = FlowAdvanceScreen;
  f.advance_page_overflow = FlowAdvancePageOverflow;
  f.flush_pending_block = FlowFlushPendingBlock;
  return f;
}

// Thin wrappers preserving the original call signatures for all existing
// call sites inside this file. Zero call-site changes required.
static void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}
static void ApplyDeferredStyleSync(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::ApplyDeferredStyleSync(p, ts);
}
static void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, MakeFlowEmissionFns());
}
static void QueueFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt,
                                   int txtlen) {
  book_xml_flow_emission::QueueFlowedFragmentRaw(p, txt, txtlen, MakeFlowEmissionFns());
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
  if (!p || !p->ts)
    return;

  AppendParsedByte(p, '\n');
  p->pen.x = p->ts->margin.left;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

static void linefeed_r(parsedata_t *p, const char *tag,
                       const char *reason, int is_real) {
  (void)tag; (void)reason; (void)is_real;
  linefeed(p);
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

static void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
}

static void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  ClearPendingBlockSpacing(p);

  Text *ts = p->ts;

  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    // Important: parser state alone is not enough. The page renderer must also
    // see a marker in the buffer so it switches from the first reading screen
    // to the second one at the same point.
    AppendScreenBreakIfNeeded(p);
    p->screen = 1;
  }

  p->current_screen_has_drawable_content = false;
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

static bool IsFigureContainerElement(const char *el, const char *class_attr) {
  if (!el)
    return false;
  if (!strcmp(el, "figure"))
    return true;
  return !strcmp(el, "div") && ClassListContains(class_attr, "figure");
}

static void FlushInlineTailBeforeElementStart(parsedata_t *p, Text *ts,
                                              const char *el) {
  if (!p)
    return;

  // Whitespace-only indentation/newlines between block elements must not be
  // emitted as real flowed text. If emitted, it can consume pending block
  // breaks before the next real block content arrives.
  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

static void FlushInlineTailBeforeElementEnd(parsedata_t *p, Text *ts,
                                            const char *el) {
  if (!p)
    return;

  // Whitespace-only indentation/newlines before closing block elements must not
  // be emitted as real flowed text. Otherwise XHTML pretty-printing between
  // </p>, </dd>, </dl>, etc. can affect line state and consume pending breaks.
  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

static void SetCurrentStackHidden(parsedata_t *p, bool hidden) {
  if (!p || p->stacksize == 0)
    return;
  p->style_hidden_stack[p->stacksize - 1] = hidden;
}

} // namespace

namespace xml {
namespace book {
namespace metadata {

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

} // namespace metadata
} // namespace book
} // namespace xml

namespace xml {
namespace book {

void chardata(void *data, const XML_Char *txt, int txtlen);

void instruction(void *data, const char *target, const char *pidata) {}

// Adapters bridging the anonymous-namespace statics into the TableHandlerFns
// callback struct so book_xml_table_handler.cpp can call them.
static void TableLf(parsedata_t *p) { linefeed(p); }
static void TableFlush(parsedata_t *p, Text *ts) {
  FlushInlineTailAndDeferredStyle(p, ts);
}
static void TableEmit(parsedata_t *p, const char *txt, int len) {
  book_xml_flow_emission::EmitFlowedFragmentRaw(p, txt, len, MakeFlowEmissionFns());
}
static TableHandlerFns MakeTableHandlerFns() {
  TableHandlerFns f;
  f.linefeed = TableLf;
  f.flush_inline_tail = TableFlush;
  f.emit_flowed_raw = TableEmit;
  return f;
}

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

// Force a mandatory block boundary before a block-level element that may be
// nested inside an inline styling wrapper (small, big, span, em, b, etc.).
//
// If linebegan==true (inline content exists on the current line), emits a
// mandatory '\n' immediately — NOT via the pending model — so the break lands
// in the buffer BEFORE any font-size or style tokens the block handler will
// emit.  Afterwards, pen.x is reset to the block left margin and linebegan is
// false, so the subsequent QueueBlockSpacingFromMarginResult call in the
// block handler will not emit a duplicate break.
//
// If linebegan==false the line is already fresh; nothing is emitted.
static void EnsureBlockBoundaryBeforeBlockStart(parsedata_t *p,
                                                const char *tag,
                                                const char *reason) {
  if (!p || !p->ts || !p->book)
    return;

  if (IsCurrentReadingScreenVisuallyEmpty(p)) {
    p->pending_block_break = false;
    return;
  }

  const int line_step = p->ts->GetHeight() + p->ts->linespacing;

  auto advance_or_linefeed = [&]() {
    if (line_step <= 0) {
      linefeed_r(p, tag, reason, 0);
      return;
    }

    const text_render_layout_utils::ReadingScreenMetrics metrics =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0,
            p->screen,
            p->ts->margin.bottom,
            MIN(p->ts->margin.bottom, 16));

    const int next_y = p->pen.y + line_step;
    const bool next_line_fits =
        text_render_layout_utils::CurrentLineFitsScreen(
            next_y,
            p->ts->GetHeight(),
            p->ts->linespacing,
            metrics.max_height,
            metrics.bottom_margin);

    if (next_line_fits) {
      linefeed_r(p, tag, reason, 0);
    } else {
      AdvanceParsedScreen(p);
    }
  };

  // Si hay un salto de bloque pendiente, hay que materializarlo sí o sí.
  if (p->pending_block_break) {
    advance_or_linefeed();
    p->pending_block_break = false;
    return;
  }

  const bool buffer_already_broken =
      p->buflen > 0 && p->buf[p->buflen - 1] == '\n';

  const bool needs_boundary =
      p->linebegan ||
      (p->buflen > 0 &&
       !buffer_already_broken &&
       !IsCurrentReadingScreenVisuallyEmpty(p));

  if (!needs_boundary)
    return;

  advance_or_linefeed();
}

static void HeadingEnsureBlockBoundary(parsedata_t *p, const char *tag,
                                       const char *phase) {
  EnsureBlockBoundaryBeforeBlockStart(p, tag, phase);
}
static void HeadingAdvanceScreen(parsedata_t *p) { AdvanceParsedScreen(p); }
static void HeadingQueueBlockSpacing(
    parsedata_t *p, const char *tag, const char *phase,
    const book_xml_css_style_utils::MarginTopResult &mtr, int line_h,
    int default_lf) {
  QueueBlockSpacingFromMarginResult(p, tag, phase, mtr, line_h, default_lf);
}
static HeadingHandlerFns MakeHeadingHandlerFns() {
  HeadingHandlerFns f;
  f.ensure_block_boundary = HeadingEnsureBlockBoundary;
  f.advance_screen = HeadingAdvanceScreen;
  f.queue_block_spacing = HeadingQueueBlockSpacing;
  return f;
}

static void AnchorLf(parsedata_t *p) { linefeed(p); }
static AnchorHandlerFns MakeAnchorHandlerFns() {
  AnchorHandlerFns f;
  f.linefeed = AnchorLf;
  return f;
}

static void ImageLf(parsedata_t *p) { linefeed(p); }
static void ImageAdvanceScreen(parsedata_t *p) { AdvanceParsedScreen(p); }
static void ImageAdvancePageOverflow(parsedata_t *p, int lineheight) {
  AdvanceParsedPageOnOverflow(p, lineheight);
}
static void ImageEmitChardata(parsedata_t *p, const char *txt, int len) {
  chardata((void *)p, txt, len);
}
static ImageHandlerFns MakeImageHandlerFns() {
  ImageHandlerFns f;
  f.linefeed = ImageLf;
  f.advance_screen = ImageAdvanceScreen;
  f.advance_page_overflow = ImageAdvancePageOverflow;
  f.emit_chardata = ImageEmitChardata;
  return f;
}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  ElementPerfScope elem_perf(p);
  Text *ts = p->ts;
  FlushInlineTailBeforeElementStart(p, ts, el);

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

  if (HandleTableStart(p, ts, el, attr, MakeTableHandlerFns()))
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
  epub_css_class_map::LookupAllForTag(el, p->css_class_map, &elem_css);
  epub_css_class_map::MergeClassRulesToStyle(
      el_class_raw ? std::string(el_class_raw) : std::string(),
      p->css_class_map, &elem_css);

  if (BehavesAsBlock(el, elem_css)) {
    if (ParseElementClear(attr, elem_css) !=
        book_xml_css_style_utils::ClearMode::None) {
      ApplyClearBreak(p);
    }
    if (!IsFigureContainerElement(el, el_class_raw) &&
        (book_xml_css_style_utils::HasPageBreakInsideAvoid(el_style_raw) ||
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
    QueueBlockSpacingLines(p, 1, "aside", "aside-top", false);
  } else if (!strcmp(el, "blockquote")) {
    parse_push(p, TAG_BLOCKQUOTE);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    QueueBlockSpacingLines(p, 1, "blockquote", "blockquote-top", false);
  } else if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    QueueBlockSpacingLines(p, 1, "caption", "caption-top", false);
  } else if (!strcmp(el, "dd")) {
    parse_push(p, TAG_DD);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    if (elem_css.margin_left.unit ==
        book_xml_css_style_utils::MarginTopResult::Unit::None) {
      const int space_advance = ts->GetAdvance(' ');
      const int legacy_dd_indent_px =
          space_advance > 0 ? 2 * space_advance : 12;

      parse_set_current_block_margins(
          p,
          parse_current_block_margin_left(p) + legacy_dd_indent_px,
          parse_current_block_margin_right(p));
    }

    QueueBlockSpacingLines(p, 1, "dd", "dd-top", false);
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
    QueueBlockSpacingLines(p, 1, "figure", "figure-top", false);
  }
  else if (!strcmp(el, "h1")) {
    HandleHeadingStart(p, ts, attr, elem_css, 1, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h2")) {
    HandleHeadingStart(p, ts, attr, elem_css, 2, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h3")) {
    HandleHeadingStart(p, ts, attr, elem_css, 3, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h4")) {
    parse_push(p, TAG_H4);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 4, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map, "h4"));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h4", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "h4", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "h5")) {
    parse_push(p, TAG_H5);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 5, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map, "h5"));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h5", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "h5", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "h6")) {
    parse_push(p, TAG_H6);
    p->last_h_style = ExtractStyleAttr(attr);
    p->last_h_class = ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 6, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, ResolveElementTextAlignWithClass(p->last_h_style, p->last_h_class,
                                            p, p->css_class_map, "h6"));
    AppendParsedByte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          ParseElementMarginTopWithClass(attr, elem_css);
      ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h6", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "h6", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "head"))
    parse_push(p, TAG_HEAD);
  else if (!strcmp(el, "ol")) {
    parse_push(p, TAG_OL);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  } else if (!strcmp(el, "p")) {
    EnsureBlockBoundaryBeforeBlockStart(p, "p", "paragraph-block-boundary");
    parse_push(p, TAG_P);
    p->in_paragraph = true;
    p->paragraph_has_content = false;
    p->text_transform_word_start = true;
    p->last_p_style = ExtractStyleAttr(attr);
    p->last_p_class = ExtractClassAttr(attr);
    const book_xml_css_style_utils::TextAlign align =
        ResolveElementTextAlignWithClass(p->last_p_style, p->last_p_class,
                                         p, p->css_class_map, "p");
    AppendParagraphAlignMarker(p, align);
    const bool tight_list_paragraph =
        book_xml_list_utils::HasPendingListItemContent(p);
    const bool tight_block_paragraph = ParseInAnyEasyParagraphTightBlock(p);
    const bool can_apply_top_margin =
        !tight_list_paragraph && !tight_block_paragraph;
    const book_xml_css_style_utils::MarginTopResult mtr =
        ParseElementMarginTopWithClass(attr, elem_css);
    ApplyElementBlockMargins(p, ts, attr, elem_css);
    const int line_h = ts->GetHeight() + ts->linespacing;
    if (can_apply_top_margin) {
      const int default_lf = p->book->GetParagraphSpacing();
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "p", "top", p->last_p_style,
                             p->last_p_class, mtr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "p", "paragraph-top", mtr, line_h, default_lf);
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
    QueueBlockSpacingFromMarginResult(p, "hr", "hr-top", mtr, line_h, default_lf);
    if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
      FlushPendingBlockSpacingBeforeContent(p, "hr");
      // Compute rule bounds: block margins (from margin-left/right CSS) narrow
      // the content area; width CSS + alignment determine the rule width within
      // that area.  For plain <hr/> all margins are 0 and the bounds match the
      // old TEXT_HR full-width behavior exactly.
      const int content_left =
          ts->margin.left + parse_current_block_margin_left(p);
      const int content_right =
          ts->display.width - ts->margin.right - parse_current_block_margin_right(p);
      const int content_w = std::max(0, content_right - content_left);

      int rule_x0 = content_left;
      int rule_x1 = content_right;

      const book_xml_css_style_utils::MarginTopResult width_spec =
          book_xml_css_style_utils::ParseWidth(p->last_hr_style.c_str());
      if (width_spec.unit != book_xml_css_style_utils::MarginTopResult::Unit::None &&
          !width_spec.negative && content_w > 0) {
        const int css_w = book_xml_css_style_utils::ResolveHorizontalMarginPx(
            width_spec, content_w);
        const int rule_w = std::max(1, std::min(css_w, content_w));
        // Alignment: inline style > class > center (hr HTML default)
        book_xml_css_style_utils::TextAlign align =
            book_xml_css_style_utils::TextAlign::Center;
        book_xml_css_style_utils::TextAlign from_style;
        if (book_xml_css_style_utils::TryParseTextAlign(
                p->last_hr_style.c_str(), &from_style)) {
          align = from_style;
        } else if (elem_css.has_text_align) {
          align = elem_css.text_align;
        }
        if (align == book_xml_css_style_utils::TextAlign::Left ||
            align == book_xml_css_style_utils::TextAlign::Justify) {
          rule_x0 = content_left;
          rule_x1 = content_left + rule_w;
        } else if (align == book_xml_css_style_utils::TextAlign::Right) {
          rule_x1 = content_right;
          rule_x0 = content_right - rule_w;
        } else {
          const int mid = (content_left + content_right) / 2;
          rule_x0 = mid - rule_w / 2;
          rule_x1 = rule_x0 + rule_w;
        }
        rule_x0 = std::max(content_left, std::min(content_right, rule_x0));
        rule_x1 = std::max(rule_x0, std::min(content_right, rule_x1));
      }

      // Clamp to u8: display.width <= 240px on 3DS, so all positions fit.
      const u32 x0_u = (u32)std::max(0, std::min(255, rule_x0));
      const u32 x1_u = (u32)std::max(x0_u, (u32)std::min(255, rule_x1));

      AppendParsedByte(p, TEXT_HR_BOUNDS);
      AppendParsedByte(p, x0_u);
      AppendParsedByte(p, x1_u);
      // HR renders as a visible rule — mark the screen as having drawable content.
      p->current_screen_has_drawable_content = true;
      // The renderer calls PrintNewLine() for TEXT_HR_BOUNDS, advancing pen.y
      // by one line. Mirror that here so overflow tracking stays in sync.
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
    if (HasActiveStackHiddenStyle(p))
      return;
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
  } else if (!strcmp(el, "ruby")) {
    parse_push(p, TAG_RUBY);
  } else if (!strcmp(el, "rp")) {
    // <rp> provides fallback parens for non-ruby renderers; we add our own
    // around <rt>, so suppress <rp> content entirely.
    parse_push(p, TAG_RP);
    SetCurrentStackHidden(p, true);
  } else if (!strcmp(el, "rt")) {
    parse_push(p, TAG_RT);
    if (!HasActiveStackHiddenStyle(p)) {
      // Render annotation as (text) at ~75% size.
      chardata(p, "(", 1);
      const u8 current = (u8)(p->stacksize - 1);
      const u8 saved_px = ts->GetPixelSize();
      const u8 small_px = (u8)book_xml_parser_style_utils::ClampInlineFontSize(
          p->base_font_size_px, (int)(saved_px * 3 / 4));
      if (small_px != saved_px) {
        p->style_font_size_stack[current] = small_px;
        p->style_font_size_restore_stack[current] = saved_px;
        ts->SetPixelSize(small_px);
        AppendParsedByte(p, TEXT_FONT_SIZE);
        AppendParsedByte(p, (u32)small_px);
      }
    }
  } else if (!strcmp(el, "a")) {
    HandleAnchorStart(p, attr);
  } else if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
    HandleInlineImageStart(p, ts, attr, elem_css, MakeImageHandlerFns());
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
    QueueBlockSpacingFromMarginResult(p, el, "block-top", mtr, line_h, default_lf);
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
    if (elem_css.is_display_none)
      style_hidden = true;

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
        bool has_spec = false;
        // Publisher CSS font-size (inline style= and class-based) is gated
        // behind the respect_publisher_font_size preference. When off (User
        // size mode), only <small>/<big> semantic tags are allowed to change
        // inline font size. Element selectors such as body/p/div from
        // stylesheets are not resolved here and are therefore unaffected.
        const bool use_publisher_font_size =
            p->prefs && p->prefs->respect_publisher_font_size;
        if (use_publisher_font_size) {
          has_spec = book_xml_css_style_utils::TryParseFontSize(
              ExtractStyleAttr(attr).c_str(), &spec);
          if (!has_spec && elem_css.font_size.unit != book_xml_css_style_utils::FontSizeSpec::Unit::None) {
            spec = elem_css.font_size;
            has_spec = true;
          }
        }
        // <small>/<big> and CSS keyword sizes (small, x-small, smaller, etc.)
        // are semantic choices — apply regardless of the publisher font-size preference.
        if (!has_spec) {
          if (!strcmp(el, "small")) {
            spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Smaller;
            has_spec = true;
          } else if (!strcmp(el, "big")) {
            spec.unit = book_xml_css_style_utils::FontSizeSpec::Unit::Larger;
            has_spec = true;
          } else if (elem_css.font_size.unit != book_xml_css_style_utils::FontSizeSpec::Unit::None &&
                     elem_css.font_size.is_keyword) {
            // In user-size mode the absolute CSS baseline (browser 16px) is
            // irrelevant. Remap absolute keyword percents to the same semantic
            // steps used by <small>/<big> so that "font-size: small" and
            // <small> produce identical output.
            using U = book_xml_css_style_utils::FontSizeSpec::Unit;
            if (elem_css.font_size.unit == U::Percent) {
              spec.unit = (elem_css.font_size.value_x100 < 10000) ? U::Smaller
                        : (elem_css.font_size.value_x100 > 10000) ? U::Larger
                        : U::None;
              spec.value_x100 = 0;
              spec.is_keyword = true;
            } else {
              spec = elem_css.font_size; // Smaller/Larger already semantic
            }
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
  FlushInlineTailBeforeElementEnd(p, ts, el);

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

  if (HandleTableEnd(p, ts, el, MakeTableHandlerFns()))
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
    // <br> is real content; flush any pending layout-only block spacing first.
    FlushPendingBlockSpacingBeforeContent(p, "br");
    linefeed(p);
  } else if (!strcmp(el, "a")) {
    HandleAnchorEnd(p, MakeAnchorHandlerFns());
  } else if (!strcmp(el, "aside")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    QueueBlockSpacingLines(p, 2, "aside", "aside-bottom", false);
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "blockquote") || !strcmp(el, "caption") ||
             !strcmp(el, "dd") || !strcmp(el, "figure")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    QueueBlockSpacingLines(p, 1, el, "block-bottom", false);
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
                                            p->css_class_map, "p");
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "p", "bottom", p->last_p_style,
                             p->last_p_class, mbr, line_h, default_lf,
                             lf_count);
      // Queue spacing (mandatory break + optional). CSS zero/negative keeps the
      // mandatory break but suppresses optional spacing — the block still ends on
      // its own visual line.
      QueueBlockSpacingFromMarginResult(p, "p", "paragraph-bottom", mbr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
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
                                            p->css_class_map, "h1");
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h1", "bottom", p->last_h1_style,
                             p->last_h1_class, mbr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "h1", "heading-bottom", mbr, line_h, default_lf);
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
                                            p->css_class_map, "h2");
      const int default_lf = 1;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h2", "bottom", p->last_h2_style,
                             p->last_h2_class, mbr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, "h2", "heading-bottom", mbr, line_h, default_lf);
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
                                            p->css_class_map, "hr");
      const int default_lf = 2;
      const int lf_count =
          book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
              default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "hr", "bottom", p->last_hr_style,
                             p->last_hr_class, mbr, line_h, default_lf,
                             lf_count);
      if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
        // TEXT_HR_BOUNDS was emitted, so the renderer has linebegan=false.  It will
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
                                            p->css_class_map, el);
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, el, "bottom", p->last_h_style,
                             p->last_h_class, mbr, line_h, default_lf,
                             lf_count);
      QueueBlockSpacingFromMarginResult(p, el, "heading-bottom", mbr, line_h, default_lf);
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
    QueueBlockSpacingLines(p, 2, "pre", "block-bottom", false);
  } else if (!strcmp(el, "code") || !strcmp(el, "tt") ||
             !strcmp(el, "kbd") || !strcmp(el, "samp")) {
  } else if (!strcmp(el, "li") || !strcmp(el, "ul") || !strcmp(el, "ol")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (!strcmp(el, "li"))
      p->strip_leading_list_marker = false;
    // Only queue spacing when we're mid-line; if we're already at a line start
    // (linebegan=false), the previous linefeed already separated items.
    if (p->linebegan)
      QueueBlockSpacingLines(p, 1, el, "list-item-bottom", false);
  } else if (!IsBlockLevelElement(el) && p->stacksize > 0 &&
             p->block_text_align_stack[(u8)(p->stacksize - 1)]) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (p->linebegan)
      QueueBlockSpacingLines(p, 1, el, "block-align-bottom", false);
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
    // Emit closing paren for <rt> at the reduced annotation size, before
    // font restore fires below.
    if (p->stack[current] == TAG_RT && !HasActiveStackHiddenStyle(p))
      chardata(p, ")", 1);
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
          text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
  int maxHeight = metrics.max_height;
  int bottomMargin = metrics.bottom_margin;
  if (!text_render_layout_utils::CurrentLineFitsScreen(
        p->pen.y, ts->GetHeight(), ts->linespacing, maxHeight,
        bottomMargin)) {
    AdvanceParsedScreen(p);
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

} // namespace book
} // namespace xml
