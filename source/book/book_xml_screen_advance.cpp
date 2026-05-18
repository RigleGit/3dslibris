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
#include "shared/text_render_layout_utils.h"
#include "ui/text.h"

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
  return p && !p->current_screen_has_drawable_content;
}

void ClearPendingBlockSpacing(parsedata_t *p) {
  if (!p)
    return;
  p->pending_block_break = false;
  p->pending_block_spacing_lf = 0;
  p->pending_block_spacing_reason = NULL;
  p->pending_block_spacing_from_css = false;
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
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, leftBottomMargin,
          rightBottomMargin);
  int maxHeight = metrics.max_height;
  int bottomMargin = metrics.bottom_margin;
  if (!text_render_layout_utils::CurrentLineBeyondReadingScreen(
          p->pen.y, maxHeight, bottomMargin))
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
    book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
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

void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  ClearPendingBlockSpacing(p);

  Text *ts = p->ts;

  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
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
#ifdef DSLIBRIS_DEBUG
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
#ifdef DSLIBRIS_DEBUG
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
#ifdef DSLIBRIS_DEBUG
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
    SuppressPendingBlockSpacingFromCss(p, tag, reason);
    return;
  }
  // Snapshot suppress_only before the queue clears it: if the preceding element
  // explicitly suppressed its bottom margin (via CSS margin: 0), we need to
  // restore one line of spacing after this element's CSS margin-top queues.
  const bool was_suppressed = p->pending_block_spacing_suppress_only;
  const int css_lf = book_xml_parser_style_utils::ResolveCssMarginLinefeeds(mtr, line_h);
  const int resolved = std::max(css_lf, default_lf);
  const int clamped = book_xml_parser_style_utils::ClampResolvedBlockLinefeeds(resolved);
  QueueBlockSpacingLines(p, clamped, tag, reason, true);
  // After a CSS suppress → CSS margin-top sequence, ensure at least one optional
  // spacing line so the stanza/section separator renders visibly.
  if (was_suppressed && p->pending_block_spacing_lf < 1)
    p->pending_block_spacing_lf = 1;
}

void FlushPendingBlockSpacingBeforeContent(parsedata_t *p,
                                           const char *next_tag) {
  if (!p || !p->ts || !p->book)
    return;
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(p->book->GetStatusReporter(),
    "FlushPending ENTER[->%s] pbb=%d pbl=%d from_css=%d pen_y=%d lb=%d scr=%d vis=%d",
    next_tag ? next_tag : "?",
    p->pending_block_break ? 1 : 0,
    p->pending_block_spacing_lf,
    p->pending_block_spacing_from_css ? 1 : 0,
    p->pen.y, p->linebegan ? 1 : 0, p->screen,
    p->current_screen_has_drawable_content ? 1 : 0);
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
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
  int available = 0;
  {
    const int usable = metrics.max_height - metrics.bottom_margin - p->pen.y;
    available = (usable > 0) ? (usable / line_step) : 0;
  }

  // ---- Phase 1: mandatory block break -----------------------------------
  if (p->pending_block_break && p->linebegan) {
    if (available >= 1) {
      LinefeedRLocal(p, next_tag ? next_tag : "?", "mandatory-break", 0);
      available--;
    } else {
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
      LinefeedRLocal(p, next_tag ? next_tag : "?", "pending-spacing", 0);
  }
#ifdef DSLIBRIS_DEBUG
  DBG_LOGF(p->book->GetStatusReporter(),
    "FlushPending EXIT[->%s] opt=%d avail=%d emit_opt=%d pen_y=%d scr=%d lb=%d",
    next_tag ? next_tag : "?",
    opt, available, emit_opt, p->pen.y, p->screen, p->linebegan ? 1 : 0);
#endif

  ClearPendingBlockSpacing(p);
}

} // namespace book_xml_screen_advance
