#include "book/book_xml_flow_layout.h"

#include "book/book.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/page.h"
#include "parse.h"
#include "shared/text_render_layout_utils.h"
#include "shared/text_token_constants.h"
#include <sys/param.h>

namespace book_xml_flow_layout {

void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight) {
  if (!p || !p->book || !p->ts)
    return;

  Text *ts = p->ts;
  const int leftBottomMargin = ts->margin.bottom;
  const int rightBottomMargin =
    text_render_layout_utils::ResolveCompactReadingBottomMargin(
        ts->margin.bottom);
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
    book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + lineheight;
}

void AdvanceParsedPageOnOverflowThunk(parsedata_t *p, int lineheight,
                                      void *ctx) {
  (void)ctx;
  AdvanceParsedPageOnOverflow(p, lineheight);
}

void AdvanceParsedScreen(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;

  Text *ts = p->ts;
  if (p->screen == 1) {
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
    p->screen = 0;
  } else {
    p->screen = 1;
  }

  p->pen.x = ts->margin.left;
  p->pen.y = ts->margin.top + ts->GetHeight();
  p->linebegan = false;
}

void ForcePageBreak(parsedata_t *p) {
  if (!p || !p->ts || !p->book)
    return;
  if (p->buflen == 0)
    return;
  if (p->screen == 0) {
    parse_append_page_byte(p, TEXT_SCREEN_BREAK);
  }
  AdvanceParsedScreen(p);
}

} // namespace book_xml_flow_layout
