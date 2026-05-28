/*
    3dslibris - book_xml_parser.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Expat XML callbacks (start/chardata/end) for EPUB/FB2/HTML content.
    - Metadata-only parser for title extraction (xml::book::metadata).
    - Adapter shims that wire handler modules into the callback structs.
    - Support helpers (path utils, inline-state wrappers) are in
      book_xml_parser_support.cpp.
*/

#include "book/book.h"

#include "book/book_xml_parser_support.h"
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
#include "book/book_xml_element_style.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_inline_handler.h"
#include "book/book_xml_block_handler.h"
#include "book/book_xml_fb2_handler.h"
#include "book/book_xml_screen_advance.h"
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

using namespace book_xml_parser_support;

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
  return book_xml_element_style::ParseElementClear(attr, elem_css);
}
static book_xml_css_style_utils::FloatMode
ParseElementFloat(const char **attr,
                  const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementFloat(attr, elem_css);
}
static u8 ParseElementTextTransform(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementTextTransform(attr, elem_css);
}
static u8 ParseElementWhiteSpace(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementWhiteSpace(attr, elem_css);
}
static void ApplyElementBlockMargins(
    parsedata_t *p, Text *ts, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
}
static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopWithClass(const char **attr,
                               const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
}
static void ConfigureBlockTextAlign(
    parsedata_t *p, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  book_xml_element_style::ConfigureBlockTextAlign(p, el, attr, elem_css);
}
static void EnsureBlockBoundaryBeforeBlockStart(parsedata_t *p,
                                                const char *tag,
                                                const char *reason) {
  book_xml_element_style::EnsureBlockBoundaryBeforeBlockStart(p, tag, reason);
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

// Forward declaration — chardata is defined later in this namespace.
void chardata(void *data, const XML_Char *txt, int txtlen);

static void InlineEmitChardata(parsedata_t *p, const char *txt, int len) {
  chardata(p, txt, len);
}
static InlineHandlerFns MakeInlineHandlerFns() {
  InlineHandlerFns f;
  f.emit_chardata = InlineEmitChardata;
  return f;
}

static bool ElementIsHidden(const char **attr,
                            const epub_css_class_map::CssClassMargins &elem_css) {
  bool hidden = false;
  book_xml_css_resolver::ParseElementHiddenFlags(attr, &hidden);
  return hidden || elem_css.is_display_none;
}

static bool IsParserStructuralElement(const char *el) {
  return el && (!strcmp(el, "html") || !strcmp(el, "body") ||
                !strcmp(el, "head") || !strcmp(el, "title") ||
                !strcmp(el, "script") || !strcmp(el, "style"));
}

static context_t HiddenContextForElement(const char *el) {
  if (!el)
    return TAG_UNKNOWN;
  if (!strcmp(el, "h1"))
    return TAG_H1;
  if (!strcmp(el, "h2"))
    return TAG_H2;
  if (!strcmp(el, "h3"))
    return TAG_H3;
  if (!strcmp(el, "h4"))
    return TAG_H4;
  if (!strcmp(el, "h5"))
    return TAG_H5;
  if (!strcmp(el, "h6"))
    return TAG_H6;
  if (!strcmp(el, "p"))
    return TAG_P;
  if (!strcmp(el, "div"))
    return TAG_DIV;
  if (!strcmp(el, "aside"))
    return TAG_ASIDE;
  if (!strcmp(el, "blockquote"))
    return TAG_BLOCKQUOTE;
  if (!strcmp(el, "figure"))
    return TAG_FIGURE;
  if (!strcmp(el, "ol"))
    return TAG_OL;
  if (!strcmp(el, "ul"))
    return TAG_UL;
  if (!strcmp(el, "li"))
    return TAG_LI;
  if (!strcmp(el, "pre"))
    return TAG_PRE;
  if (!strcmp(el, "a"))
    return TAG_ANCHOR;
  if (!strcmp(el, "br"))
    return TAG_BR;
  if (!strcmp(el, "table"))
    return TAG_TABLE;
  if (!strcmp(el, "tr"))
    return TAG_TR;
  if (!strcmp(el, "td"))
    return TAG_TD;
  if (!strcmp(el, "th"))
    return TAG_TH;
  return TAG_UNKNOWN;
}

static bool PushHiddenElement(parsedata_t *p, const char *el) {
  if (!p || !el || IsParserStructuralElement(el))
    return false;
  parse_push(p, HiddenContextForElement(el));
  SetCurrentStackHidden(p, true);
  return true;
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

  book_xml_fb2_handler::HandleFb2SectionStart(p, el);

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

  if (HasActiveStackHiddenStyle(p) || ElementIsHidden(attr, elem_css)) {
    if (PushHiddenElement(p, el))
      return;
  }

  if (HandleTableStart(p, ts, el, attr, MakeTableHandlerFns()))
    return;

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
      ForceHardPageBreak(p);
    }

    if (el_style_raw && el_style_raw[0]) {
      const book_xml_css_style_utils::MarginTopResult ptr =
          book_xml_css_style_utils::ParsePaddingTop(el_style_raw);
      if (ptr.unit != book_xml_css_style_utils::MarginTopResult::Unit::None &&
          !ptr.negative && ptr.value > 0) {
        const book_xml_css_style_utils::MarginTopResult mtr =
            book_xml_css_style_utils::ParseMarginTop(el_style_raw);
        if (mtr.unit == book_xml_css_style_utils::MarginTopResult::Unit::None) {
          const int line_h = ts->GetHeight() + ts->linespacing;
          QueueBlockSpacingFromMarginResult(
              p, el, "padding-top-inline", ptr, line_h, 0);
        }
      }
    }
  }

  if (!strcmp(el, "h1")) {
    HandleHeadingStart(p, ts, attr, elem_css, 1, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h2")) {
    HandleHeadingStart(p, ts, attr, elem_css, 2, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h3")) {
    HandleHeadingStart(p, ts, attr, elem_css, 3, MakeHeadingHandlerFns());
  } else if (XmlNameEquals(el, "title")) {
    parse_push(p, TAG_TITLE);
  } else {
    bool block_early_return = false;
    if (book_xml_block_handler::HandleBlockElementStart(
            p, ts, el, attr, elem_css, el_class_raw, &block_early_return)) {
      if (block_early_return) return;
    } else if (book_xml_inline_handler::HandleNamedInlineElementStart(
                   p, ts, el, attr, MakeInlineHandlerFns())) {
      // handled
    } else if (!strcmp(el, "a")) {
      HandleAnchorStart(p, attr);
    } else if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
      HandleInlineImageStart(p, ts, attr, elem_css, MakeImageHandlerFns());
    } else if (book_xml_fb2_handler::HandleFb2BinaryStart(p, el, attr)) {
      // handled
    } else {
      parse_push(p, TAG_UNKNOWN);
    }
  }

  ConfigureBlockTextAlign(p, el, attr, elem_css);

  book_xml_block_handler::ApplyDisplayBlockPromotion(p, ts, el, attr, elem_css);

  book_xml_inline_handler::HandleCssInlineStylingStart(p, ts, el, attr, elem_css);

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

  if (book_xml_fb2_handler::HandleFb2Chardata(p, (const char *)txt, txtlen))
    return;
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

  if (book_xml_fb2_handler::HandleFb2BinaryEnd(p, el))
    return;

  book_xml_fb2_handler::HandleFb2TitleSectionEnd(p, el);

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

  const bool current_element_hidden =
      p->stacksize > 0 && p->style_hidden_stack[p->stacksize - 1];
  if (current_element_hidden) {
    parse_pop(p);
    return;
  }

  if (HandleTableEnd(p, ts, el, MakeTableHandlerFns()))
    return;

  if (!strcmp(el, "a")) {
    HandleAnchorEnd(p, MakeAnchorHandlerFns());
  } else if (book_xml_block_handler::HandleBlockElementEnd(p, ts, el)) {
    // handled
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
    const int old_lh = (int)ts->GetHeight();
    ts->SetPixelSize(restore_font_size_px);
    const int new_lh = (int)ts->GetHeight();
    // If pen.y was placed at the start of a fresh screen using the inflated
    // font's lineheight (e.g. by advance_page_overflow during a BAND image),
    // and the element is not mid-line, correct pen.y to the restored font's
    // lineheight so subsequent spacing decisions use the right baseline.
    if (!p->linebegan && new_lh < old_lh &&
        p->pen.y == (int)ts->margin.top + old_lh) {
      p->pen.y = (int)ts->margin.top + new_lh;
    }
    AppendParsedByte(p, TEXT_FONT_SIZE);
    AppendParsedByte(p, restore_font_size_px);
    // Block-spacing suppress state from inside a font-size scope (e.g. a
    // zero-margin image container inside a 200%-font div) must not propagate
    // past the scope boundary.  The next block element after this restore
    // should not inherit the suppress signal and add an unintended blank line.
#if defined(DSLIBRIS_DEBUG)
    DBG_LOGF(p->book->GetStatusReporter(),
      "FontScope EXIT[%s] restore_px=%d old_lh=%d new_lh=%d suppress_only=%d->0 pbl=%d pbb=%d",
      el, (int)restore_font_size_px, old_lh, new_lh,
      p->pending_block_spacing_suppress_only ? 1 : 0,
      p->pending_block_spacing_lf,
      p->pending_block_break ? 1 : 0);
#endif
    p->pending_block_spacing_suppress_only = false;
  }

  book_xml_inline_handler::SyncInlineStyleAfterPop(p, ts);
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
