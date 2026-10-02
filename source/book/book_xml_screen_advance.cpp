/*
    3dslibris - book_xml_screen_advance.cpp

    Screen advancement and block spacing queue extracted from book_xml_parser.cpp.
*/

#include "book/book_xml_screen_advance.h"

#include "book/book.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/page.h"
#include "parse.h"
#include "shared/debug_log.h"
#include "shared/orientation_utils.h"
#include "shared/text_render_layout_utils.h"
#include "ui/text.h"

#include <cstring>

#ifndef BLOCK_SPACING_TRACE
#define BLOCK_SPACING_TRACE 0
#endif

#ifndef EPUB_SPACING_TRACE
#define EPUB_SPACING_TRACE 0
#endif

namespace {

static void AppendScreenBreakIfNeeded(parsedata_t *p) {
  if (!p)
    return;
  if (p->buflen <= 0)
    return;
  if (p->buf[p->buflen - 1] == TEXT_SCREEN_BREAK)
    return;
  parse_append_page_byte(p, TEXT_SCREEN_BREAK);
}

static void LinefeedRLocal(parsedata_t *p, const char *tag,
                           const char *reason, int is_real) {
  (void)tag; (void)reason; (void)is_real;
  book_xml_screen_advance::Linefeed(p);
}

static int CountRenderableLineSlots(int pen_y, int max_height,
                                    int bottom_margin, int line_step) {
  if (line_step <= 0)
    return 0;
  int slots = 0;
  int y = pen_y;
  while (!text_render_layout_utils::CurrentLineBeyondReadingScreen(
      y, max_height, bottom_margin)) {
    slots++;
    y += line_step;
  }
  return slots;
}

static int CountFloorLineSlots(int pen_y, int max_height,
                               int bottom_margin, int line_step) {
  if (line_step <= 0)
    return 0;
  const int usable = max_height - bottom_margin - pen_y;
  return (usable > 0) ? (usable / line_step) : 0;
}

} // namespace

namespace book_xml_screen_advance {

void Linefeed(parsedata_t *p) {
  if (!p || !p->ts)
    return;
  parse_append_page_byte(p, '\n');
  p->pen.x = p->ts->margin.left;
  p->pen.y += p->ts->GetHeight() + p->ts->linespacing;
  p->linebegan = false;
}

bool Blankline(parsedata_t *p) {
  if (p->buflen < 3)
    return true;
  return (p->buf[p->buflen - 1] == '\n') && (p->buf[p->buflen - 2] == '\n');
}

void ApplyClearBreak(parsedata_t *p) {
  if (!p || !p->linebegan || Blankline(p))
    return;
  Linefeed(p);
}

bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p) {
  return p && !p->current_screen_has_drawable_content && !p->linebegan;
}

void ClearPendingBlockSpacing(parsedata_t *p) {
  if (!p)
    return;
  p->pending_block_break = false;
  p->pending_block_spacing_lf = 0;
  p->pending_block_spacing_reason = NULL;
  p->pending_block_spacing_from_css = false;
  p->pending_block_spacing_advance_ok = false;
  p->pending_block_spacing_suppress_only = false;
}

void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight) {
  if (!p || !p->book || !p->ts)
    return;

  Text *ts = p->ts;
  const int leftBottomMargin = ts->margin.bottom;
  const int rightBottomMargin =
      text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom);
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForOrientation(
          p->book->GetOrientation(), p->screen, leftBottomMargin,
          rightBottomMargin);
  int maxHeight = metrics.max_height;
  int bottomMargin =
      text_render_layout_utils::ApplyLineHeightPaginationGuard(
          metrics.bottom_margin, lineheight);
  if (!text_render_layout_utils::CurrentLineBeyondReadingScreen(
          p->pen.y, maxHeight, bottomMargin))
    return;

  p->perf_page_overflows++;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    page->SetSearchEvents(p->search_events);
    page->start = p->pos;
    p->pos += p->buflen;
    page->end = p->pos;
    p->pagecount++;

    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    AppendScreenBreakIfNeeded(p);
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + lineheight;
  p->linebegan = false;
  p->current_screen_has_drawable_content = false;
  // Pending spacing accumulated before the overflow is no longer relevant;
  // we are at the top of a fresh screen/page.
  ClearPendingBlockSpacing(p);
}

void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  ClearPendingBlockSpacing(p);

  Text *ts = p->ts;

  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    page->SetSearchEvents(p->search_events);
    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
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

void ForcePageBreak(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;
  // If nothing is buffered yet we are already at the top of a fresh screen.
  if (p->buflen == 0)
    return;
  if (p->screen == 0) {
    // Emit a marker so page.cpp knows to switch to screen=1 at this position.
    parse_append_page_byte(p, TEXT_SCREEN_BREAK);
  }
  AdvanceParsedScreen(p);
}

void ForceHardPageBreak(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  if (p->buflen == 0 && p->screen == 0)
    return;

  if (p->screen == 0)
    AdvanceParsedScreen(p);

  if (p->screen == 1)
    AdvanceParsedScreen(p);
}

void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                            const char *reason, bool from_css) {
  if (!p || lines <= 0)
    return;
  p->pending_block_break = true;
  p->pending_block_spacing_suppress_only = false;
  const int new_opt = lines - 1;
  const int prev_opt = p->pending_block_spacing_lf;
  // When CSS explicitly specifies spacing after non-CSS default accumulation,
  // CSS takes precedence (margin collapsing: CSS value overrides default).
  const bool css_overrides_default = from_css && !p->pending_block_spacing_from_css;
  const int next_opt = css_overrides_default
      ? new_opt
      : (new_opt > prev_opt ? new_opt : prev_opt);
  p->pending_block_spacing_lf = next_opt;
  if (reason)
    p->pending_block_spacing_reason = reason;
  if (from_css)
    p->pending_block_spacing_from_css = true;
  // advance_ok tracks whether CSS itself contributed new_opt > 0 without a
  // subsequent user-spacing override.  Phase 2 only calls AdvanceParsedScreen
  // when both css_sourced and advance_ok are true, preventing spurious screen
  // advances when a tiny CSS bottom margin (new_opt==0) sets from_css and the
  // user spacing later raises opt to 1.
  if (from_css && new_opt > 0)
    p->pending_block_spacing_advance_ok = true;
  else if (!from_css)
    p->pending_block_spacing_advance_ok = false;
#if defined(DSLIBRIS_DEBUG) && EPUB_SPACING_TRACE
  if (from_css) {
    DBG_LOGF_CAT(
        p->book->GetStatusReporter(), DBG_LEVEL_DEBUG, DBG_CAT_EPUB,
        "SPTRACE QUEUE tag=%s reason=%s lines=%d opt=%d css=1 pen_y=%d lb=%d scr=%d vis=%d",
        tag ? tag : "?", reason ? reason : "?", lines,
        p->pending_block_spacing_lf, p->pen.y, p->linebegan ? 1 : 0,
        p->screen, p->current_screen_has_drawable_content ? 1 : 0);
  }
#endif
#if defined(DSLIBRIS_DEBUG) && BLOCK_SPACING_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "Queue[%s/%s] lines=%d opt=%d->%d from_css=%d pen_y=%d lb=%d",
    tag ? tag : "?", reason ? reason : "?",
    lines, prev_opt, next_opt, from_css ? 1 : 0,
    p->pen.y, p->linebegan ? 1 : 0);
#else
  (void)tag;
#endif
}

void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                        const char *reason) {
  if (!p)
    return;
#if defined(DSLIBRIS_DEBUG) && BLOCK_SPACING_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "Suppress[%s/%s] pbb=%d pbl=%d->0 pen_y=%d lb=%d",
    tag ? tag : "?", reason ? reason : "?",
    p->pending_block_break ? 1 : 0,
    p->pending_block_spacing_lf,
    p->pen.y, p->linebegan ? 1 : 0);
#else
  (void)tag; (void)reason;
#endif
  p->pending_block_spacing_lf = 0;
  p->pending_block_spacing_reason = "css-suppress";
  p->pending_block_spacing_from_css = true;
  p->pending_block_spacing_suppress_only = true;
  // pending_block_break is intentionally NOT cleared.
}

void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf) {
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
#if defined(DSLIBRIS_DEBUG) && FLUSHPENDING_TRACE
  {
    const char *unit_str = (mtr.unit == Unit::None) ? "none" :
                           (mtr.unit == Unit::Px) ? "px" :
                           (mtr.unit == Unit::Percent) ? "%" :
                           (mtr.unit == Unit::Em) ? "em" : "?";
    DBG_LOGF(p->book->GetStatusReporter(),
      "QueueFromMargin[%s/%s]: unit=%s val=%d neg=%d lh=%d def=%d pbb=%d pbl=%d",
      tag ? tag : "?", reason ? reason : "?",
      unit_str, mtr.value, mtr.negative ? 1 : 0,
      line_h, default_lf,
      p->pending_block_break ? 1 : 0, p->pending_block_spacing_lf);
  }
#endif
  if (mtr.unit == Unit::None) {
    QueueBlockSpacingLines(p, default_lf, tag, reason, false);
    return;
  }
  if (mtr.negative || mtr.value == 0) {
    if (book_xml_parser_style_utils::ShouldZeroMarginSuppressPendingSpacing(
            reason, p->pending_block_spacing_from_css,
            p->pending_block_spacing_suppress_only,
            p->pending_block_spacing_lf)) {
      SuppressPendingBlockSpacingFromCss(p, tag, reason);
    }
    return;
  }
  // Snapshot suppress_only before the queue clears it: if the preceding element
  // explicitly suppressed its bottom margin (via CSS margin: 0), we need to
  // restore one line of spacing after this element's CSS margin-top queues.
  const bool was_suppressed = p->pending_block_spacing_suppress_only;
  const int css_lf = book_xml_parser_style_utils::ResolveCssMarginLinefeeds(mtr, line_h);
  // CSS measures blank space between blocks. The queue also counts the line
  // break ending the previous block, so include it separately; otherwise a
  // one-line margin is consumed by that mandatory break and becomes invisible.
  const int clamped = book_xml_parser_style_utils::ClampResolvedBlockLinefeeds(css_lf + 1);
  QueueBlockSpacingLines(p, clamped, tag, reason, true);
  // After a CSS suppress → CSS margin-top sequence, ensure at least one optional
  // spacing line so the stanza/section separator renders visibly.
  if (was_suppressed && p->pending_block_spacing_lf < 1)
    p->pending_block_spacing_lf = 1;
}

// FLUSHPENDING_TRACE: per-call FlushPending ENTER/METRICS/EXIT logs.
// OFF by default — fires once per block-element start, drowning the log on
// large EPUBs (542k entries observed on a 7706-page book) and seriously
// slowing parse via SD-card fflush. Set to 1 to re-enable for diagnosis.
#ifndef FLUSHPENDING_TRACE
#define FLUSHPENDING_TRACE 0
#endif

void FlushPendingBlockSpacingBeforeContent(parsedata_t *p,
                                           const char *next_tag) {
  if (!p || !p->ts || !p->book)
    return;
#if defined(DSLIBRIS_DEBUG) && FLUSHPENDING_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "FlushPending ENTER[->%s] pbb=%d pbl=%d from_css=%d adv_ok=%d pen_y=%d lb=%d scr=%d vis=%d empty=%d",
    next_tag ? next_tag : "?",
    p->pending_block_break ? 1 : 0,
    p->pending_block_spacing_lf,
    p->pending_block_spacing_from_css ? 1 : 0,
    p->pending_block_spacing_advance_ok ? 1 : 0,
    p->pen.y, p->linebegan ? 1 : 0, p->screen,
    p->current_screen_has_drawable_content ? 1 : 0,
    IsCurrentReadingScreenVisuallyEmpty(p) ? 1 : 0);
#endif
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
      text_render_layout_utils::ResolveReadingScreenMetricsForOrientation(
          p->book->GetOrientation(), p->screen, ts->margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
  int available = 0;
  int floor_available = 0;
  int screen_limit = 0;
  int usable = 0;
  {
    const int effective_bottom_margin =
        text_render_layout_utils::ApplyLineHeightPaginationGuard(
            metrics.bottom_margin, lh);
    screen_limit = metrics.max_height - effective_bottom_margin;
    usable = screen_limit - p->pen.y;
    floor_available = CountFloorLineSlots(p->pen.y, metrics.max_height,
                                          effective_bottom_margin, line_step);
    available = CountRenderableLineSlots(p->pen.y, metrics.max_height,
                                         effective_bottom_margin, line_step);
#if defined(DSLIBRIS_DEBUG) && FLUSHPENDING_TRACE
    DBG_LOGF(p->book->GetStatusReporter(),
      "FlushPending METRICS lh=%d ls=%d step=%d maxH=%d botM=%d limit=%d usable=%d floor_avail=%d slots=%d lb=%d reason=%s",
      lh, ls, line_step,
      metrics.max_height, metrics.bottom_margin,
      screen_limit, usable, floor_available, available,
      p->linebegan ? 1 : 0,
      p->pending_block_spacing_reason ? p->pending_block_spacing_reason : "-");
#endif
  }

  // ---- Phase 1: mandatory block break -----------------------------------
  bool phase1_advanced = false;
  bool phase1_emitted = false;
  if (p->pending_block_break && p->linebegan) {
    // Only emit the mandatory break when there is visible content on screen.
    // When the screen is visually empty (e.g. after a text-overflow advance
    // that left no drawable characters on the new screen), the screen break
    // itself already provides the visual separation between blocks.  An extra
    // linefeed here would produce blank space at the very top of the screen.
    if (!IsCurrentReadingScreenVisuallyEmpty(p)) {
      // When spacing originates from explicit CSS margins, avoid squeezing the
      // visual separation into the last remaining line at the bottom edge.
      // Advancing keeps paragraph rhythm consistent with paged readers.
      const int min_available = p->pending_block_spacing_from_css ? 2 : 1;
      if (available >= min_available) {
        LinefeedRLocal(p, next_tag ? next_tag : "?", "mandatory-break", 0);
        available--;
        phase1_emitted = true;
      } else {
        AdvanceParsedScreen(p);
        phase1_advanced = true;

        const text_render_layout_utils::ReadingScreenMetrics after_metrics =
            text_render_layout_utils::ResolveReadingScreenMetricsForOrientation(
                p->book->GetOrientation(),
                p->screen,
                ts->margin.bottom,
                text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));

        const int after_bottom_margin =
            text_render_layout_utils::ApplyLineHeightPaginationGuard(
                after_metrics.bottom_margin, lh);
        const int usable_after =
            after_metrics.max_height - after_bottom_margin - p->pen.y;
        (void)usable_after;
        screen_limit = after_metrics.max_height - after_bottom_margin;
        usable = screen_limit - p->pen.y;
        floor_available = CountFloorLineSlots(p->pen.y, after_metrics.max_height,
                                              after_bottom_margin,
                                              line_step);
        available = CountRenderableLineSlots(p->pen.y, after_metrics.max_height,
                                             after_bottom_margin,
                                             line_step);
      }
    }
  }
  p->pending_block_break = false;

  // ---- Phase 2: optional spacing ----------------------------------------
  const int opt = p->pending_block_spacing_lf;
  const bool css_sourced = p->pending_block_spacing_from_css;
  const bool advance_ok = p->pending_block_spacing_advance_ok;
  const bool css_top_spacing =
      css_sourced && p->pending_block_spacing_reason &&
      std::strstr(p->pending_block_spacing_reason, "paragraph-top") != NULL;
  int emit_opt = 0;
  bool collapse_opt = false;
  bool advance_screen = false;
  bool empty_skip = false;
  if (opt > 0 && !IsCurrentReadingScreenVisuallyEmpty(p)) {
    const int required = 2;
    if (opt + required <= available) {
      emit_opt = opt;
    } else if (required <= available) {
      emit_opt = available - required;
      if (emit_opt < 0) emit_opt = 0;
      collapse_opt = true;
    } else if (css_sourced && (advance_ok || css_top_spacing)) {
      // CSS-mandated spacing can't coexist with a content line on this screen.
      // Advance to the next screen; the screen break already separates blocks.
#if defined(DSLIBRIS_DEBUG)
      DBG_LOGF(p->book->GetStatusReporter(),
        "FlushPending ADVANCE[->%s] reason=%s css_sourced=1 opt=%d slots=%d floor_avail=%d limit=%d usable=%d pen_y=%d scr=%d lb=%d vis=%d phase1_adv=%d phase1_emit=%d",
        next_tag ? next_tag : "?",
        p->pending_block_spacing_reason ? p->pending_block_spacing_reason : "-",
        opt, available, floor_available, screen_limit, usable,
        p->pen.y, p->screen, p->linebegan ? 1 : 0,
        p->current_screen_has_drawable_content ? 1 : 0,
        phase1_advanced ? 1 : 0, phase1_emitted ? 1 : 0);
#endif
      AdvanceParsedScreen(p);
      advance_screen = true;
      const text_render_layout_utils::ReadingScreenMetrics after_metrics =
          text_render_layout_utils::ResolveReadingScreenMetricsForOrientation(
              p->book->GetOrientation(), p->screen, ts->margin.bottom,
              text_render_layout_utils::ResolveCompactReadingBottomMargin(
                  ts->margin.bottom));
      const int after_bottom_margin =
          text_render_layout_utils::ApplyLineHeightPaginationGuard(
              after_metrics.bottom_margin, lh);
      const int usable_after =
          after_metrics.max_height - after_bottom_margin - p->pen.y;
      (void)usable_after;
      screen_limit = after_metrics.max_height - after_bottom_margin;
      usable = screen_limit - p->pen.y;
      floor_available = CountFloorLineSlots(p->pen.y, after_metrics.max_height,
                                            after_bottom_margin,
                                            line_step);
      available = CountRenderableLineSlots(p->pen.y, after_metrics.max_height,
                                           after_bottom_margin,
                                           line_step);
      emit_opt = 0;
    }
    for (int i = 0; i < emit_opt; i++)
      LinefeedRLocal(p, next_tag ? next_tag : "?", "pending-spacing", 0);
  } else if (opt > 0 && IsCurrentReadingScreenVisuallyEmpty(p)) {
#if defined(DSLIBRIS_DEBUG) && EPUB_SPACING_TRACE
    DBG_LOGF_CAT(
        p->book->GetStatusReporter(), DBG_LEVEL_DEBUG, DBG_CAT_EPUB,
        "SPTRACE FLUSH skip-empty tag=%s opt=%d css=%d adv_ok=%d pen_y=%d lb=%d scr=%d vis=%d",
        next_tag ? next_tag : "?", opt, css_sourced ? 1 : 0,
        advance_ok ? 1 : 0, p->pen.y, p->linebegan ? 1 : 0, p->screen,
        p->current_screen_has_drawable_content ? 1 : 0);
#endif
    empty_skip = true;
  }
#if defined(DSLIBRIS_DEBUG) && EPUB_SPACING_TRACE
  if (opt > 0) {
    DBG_LOGF_CAT(
        p->book->GetStatusReporter(), DBG_LEVEL_DEBUG, DBG_CAT_EPUB,
        "SPTRACE FLUSH decision tag=%s reason=%s opt=%d emit_opt=%d collapse_opt=%d advance_screen=%d empty_skip=%d css=%d adv_ok=%d slots=%d floor_avail=%d limit=%d usable=%d pen_y=%d lb=%d scr=%d vis=%d empty=%d phase1_adv=%d phase1_emit=%d",
        next_tag ? next_tag : "?",
        p->pending_block_spacing_reason ? p->pending_block_spacing_reason : "-",
        opt, emit_opt, collapse_opt ? 1 : 0, advance_screen ? 1 : 0,
        empty_skip ? 1 : 0, css_sourced ? 1 : 0, advance_ok ? 1 : 0,
        available, floor_available, screen_limit, usable, p->pen.y,
        p->linebegan ? 1 : 0, p->screen,
        p->current_screen_has_drawable_content ? 1 : 0,
        IsCurrentReadingScreenVisuallyEmpty(p) ? 1 : 0,
        phase1_advanced ? 1 : 0, phase1_emitted ? 1 : 0);
  }
#endif
#if defined(DSLIBRIS_DEBUG) && FLUSHPENDING_TRACE
  DBG_LOGF(p->book->GetStatusReporter(),
    "FlushPending EXIT[->%s] opt=%d slots=%d floor_avail=%d emit_opt=%d collapse_opt=%d advance_screen=%d pen_y=%d scr=%d lb=%d",
    next_tag ? next_tag : "?",
    opt, available, floor_available, emit_opt, collapse_opt ? 1 : 0,
    advance_screen ? 1 : 0, p->pen.y, p->screen, p->linebegan ? 1 : 0);
#endif

  ClearPendingBlockSpacing(p);
}

} // namespace book_xml_screen_advance
