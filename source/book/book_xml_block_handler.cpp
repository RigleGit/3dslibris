/*
    3dslibris - book_xml_block_handler.cpp

    Block element start/end handling.
    Extracted from book_xml_parser.cpp.
*/

#include "book/book_xml_block_handler.h"

#include "book/book.h"
#include "book/book_xml_block_utils.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_element_style.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_heading_handler.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_list_utils.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_screen_advance.h"
#include "book/epub_css_class_map.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "parse.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"
#include "shared/text_token_constants.h"
#include "ui/text.h"

#include <algorithm>
#include <string>

namespace {

static void AppendParagraphAlignMarker(
    parsedata_t *p, book_xml_css_style_utils::TextAlign align) {
  if (!p)
    return;
  if (align == book_xml_css_style_utils::TextAlign::Center)
    parse_append_page_byte(p, TEXT_PARAGRAPH_CENTER);
  else if (align == book_xml_css_style_utils::TextAlign::Right)
    parse_append_page_byte(p, TEXT_PARAGRAPH_RIGHT);
  else
    parse_append_page_byte(p, TEXT_PARAGRAPH_LEFT);
}

static void RestoreActiveBlockTextAlignMarker(parsedata_t *p) {
  if (!p)
    return;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->block_text_align_stack[i])
      continue;
    AppendParagraphAlignMarker(
        p, (book_xml_css_style_utils::TextAlign)p->block_text_align_value_stack[i]);
    return;
  }
  AppendParagraphAlignMarker(p, book_xml_css_style_utils::TextAlign::Left);
}

static void AlignFreshLineToBlockMargin(parsedata_t *p, Text *ts) {
  if (!p || !ts)
    return;
  const int x = std::max(0, ts->margin.left + p->block_margin_left);
  if (p->pen.x == x)
    return;
  p->pen.x = x;
  if (!p->linebegan) {
    parse_append_page_byte(p, TEXT_LINE_START_X);
    parse_append_page_byte(p, (u32)x);
  }
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

static void LogResolvedBlockMargin(
    parsedata_t *p, const char *tag, const char *phase,
    const std::string &style_attr, const std::string &class_attr,
    const book_xml_css_style_utils::MarginTopResult &m,
    int line_h, int default_lf, int final_lf) {
#ifdef DSLIBRIS_DEBUG
  using Unit = book_xml_css_style_utils::MarginTopResult::Unit;
  const char *unit_str = (m.unit == Unit::None) ? "none" :
                         (m.unit == Unit::Px) ? "px" :
                         (m.unit == Unit::Percent) ? "%" :
                         (m.unit == Unit::Em) ? "em" : "?";
  DBG_LOGF(p->book->GetStatusReporter(),
    "Margin[%s/%s] cls=%s sty=%s unit=%s val=%d neg=%d lh=%d def=%d final=%d "
    "pbb=%d pbl=%d from_css=%d pen_y=%d lb=%d",
    tag, phase,
    class_attr.empty() ? "-" : class_attr.c_str(),
    style_attr.empty() ? "-" : style_attr.c_str(),
    unit_str, m.value, m.negative ? 1 : 0,
    line_h, default_lf, final_lf,
    p->pending_block_break ? 1 : 0,
    p->pending_block_spacing_lf,
    p->pending_block_spacing_from_css ? 1 : 0,
    p->pen.y, p->linebegan ? 1 : 0);
#else
  (void)p; (void)tag; (void)phase; (void)style_attr;
  (void)class_attr; (void)m; (void)line_h; (void)default_lf; (void)final_lf;
#endif
}

static FlowEmissionFns MakeLocalFlowEmissionFns() {
  FlowEmissionFns f;
  f.advance_screen = [](parsedata_t *p) {
    book_xml_screen_advance::AdvanceParsedScreen(p);
  };
  f.advance_page_overflow = [](parsedata_t *p, int lh) {
    book_xml_screen_advance::AdvanceParsedPageOnOverflow(p, lh);
  };
  f.flush_pending_block = [](parsedata_t *p, const char *tag) {
    book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, tag);
  };
  return f;
}

} // namespace

namespace book_xml_block_handler {

bool HandleBlockElementStart(
    parsedata_t *p, Text *ts, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css,
    const char *el_class_raw, bool *out_early_return) {
  if (!p || !ts || !el)
    return false;

  if (!strcmp(el, "html")) {
    parse_push(p, TAG_HTML);
  } else if (!strcmp(el, "aside")) {
    parse_push(p, TAG_ASIDE);
    p->last_aside_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_aside_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "aside", "aside-top", mtr, line_h, 1);
    }
  } else if (!strcmp(el, "blockquote")) {
    parse_push(p, TAG_BLOCKQUOTE);
    p->last_blockquote_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_blockquote_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "blockquote", "blockquote-top", mtr, line_h, 1);
    }
  } else if (!strcmp(el, "caption")) {
    parse_push(p, TAG_CAPTION);
    p->last_caption_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_caption_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "caption", "caption-top", mtr, line_h, 1);
    }
  } else if (!strcmp(el, "dd")) {
    parse_push(p, TAG_DD);
    p->last_dd_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_dd_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
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
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "dd", "dd-top", mtr, line_h, 1);
    }
  } else if (!strcmp(el, "body")) {
    parse_push(p, TAG_BODY);
  } else if (!strcmp(el, "div")) {
    parse_push(p, TAG_DIV);
    p->last_div_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_div_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "div", "div-top", mtr, line_h, 0);
    }
  } else if (!strcmp(el, "dt")) {
    parse_push(p, TAG_DT);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
  } else if (!strcmp(el, "figure")) {
    parse_push(p, TAG_FIGURE);
    p->last_figure_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_figure_class = book_xml_css_resolver::ExtractClassAttr(attr);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "figure", "figure-top", mtr, line_h, 1);
    }
  } else if (!strcmp(el, "h4")) {
    parse_push(p, TAG_H4);
    p->last_h_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_h_class = book_xml_css_resolver::ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 4, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
               p->last_h_style, p->last_h_class,
               p->block_text_align_stack, p->block_text_align_value_stack,
               p->stacksize, p->css_class_map, "h4"));
    parse_append_page_byte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !book_xml_screen_advance::Blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h4", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "h4", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "h5")) {
    parse_push(p, TAG_H5);
    p->last_h_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_h_class = book_xml_css_resolver::ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 5, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
               p->last_h_style, p->last_h_class,
               p->block_text_align_stack, p->block_text_align_value_stack,
               p->stacksize, p->css_class_map, "h5"));
    parse_append_page_byte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !book_xml_screen_advance::Blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h5", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "h5", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "h6")) {
    parse_push(p, TAG_H6);
    p->last_h_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_h_class = book_xml_css_resolver::ExtractClassAttr(attr);
    ApplyHeadingFontSize(p, ts, 6, p->last_h_style, p->last_h_class);
    AppendParagraphAlignMarker(
        p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
               p->last_h_style, p->last_h_class,
               p->block_text_align_stack, p->block_text_align_value_stack,
               p->stacksize, p->css_class_map, "h6"));
    parse_append_page_byte(p, TEXT_BOLD_ON);
    p->pos++;
    p->bold = true;
    {
      const book_xml_css_style_utils::MarginTopResult mtr =
          book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
      book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
      const int line_h = ts->GetHeight() + ts->linespacing;
      const int default_lf = !book_xml_screen_advance::Blankline(p) ? 2 : 0;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "h6", "top", p->last_h_style,
                             p->last_h_class, mtr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "h6", "heading-top", mtr, line_h, default_lf);
      if (p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
    }
  } else if (!strcmp(el, "head")) {
    parse_push(p, TAG_HEAD);
  } else if (!strcmp(el, "ol")) {
    parse_push(p, TAG_OL);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  } else if (!strcmp(el, "p")) {
    if (!p->strip_leading_list_marker)
      book_xml_element_style::EnsureBlockBoundaryBeforeBlockStart(
          p, "p", "paragraph-block-boundary");
    parse_push(p, TAG_P);
    p->in_paragraph = true;
    p->paragraph_has_content = false;
    p->text_transform_word_start = true;
    p->last_p_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_p_class = book_xml_css_resolver::ExtractClassAttr(attr);
    const book_xml_css_style_utils::TextAlign align =
        book_xml_css_resolver::ResolveElementTextAlignWithClass(
            p->last_p_style, p->last_p_class,
            p->block_text_align_stack, p->block_text_align_value_stack,
            p->stacksize, p->css_class_map, "p");
    AppendParagraphAlignMarker(p, align);
    const bool tight_list_paragraph =
        book_xml_list_utils::HasPendingListItemContent(p);
    const bool tight_block_paragraph = ParseInAnyEasyParagraphTightBlock(p);
    const bool can_apply_top_margin =
        !tight_list_paragraph && !tight_block_paragraph;
    const book_xml_css_style_utils::MarginTopResult mtr =
        book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    const int line_h = ts->GetHeight() + ts->linespacing;
    if (can_apply_top_margin) {
      const int default_lf = p->book->GetParagraphSpacing();
      const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
          default_lf, mtr, line_h);
      LogResolvedBlockMargin(p, "p", "top", p->last_p_style,
                             p->last_p_class, mtr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "p", "paragraph-top", mtr, line_h, default_lf);
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(p->book->GetStatusReporter(),
        "P-START cls=%s pbb=%d pbl=%d from_css=%d pen_y=%d lb=%d scr=%d buflen=%d",
        p->last_p_class.empty() ? "-" : p->last_p_class.c_str(),
        p->pending_block_break ? 1 : 0,
        p->pending_block_spacing_lf,
        p->pending_block_spacing_from_css ? 1 : 0,
        p->pen.y, p->linebegan ? 1 : 0, p->screen, p->buflen);
#endif
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
    p->last_hr_style = book_xml_css_resolver::ExtractStyleAttr(attr);
    p->last_hr_class = book_xml_css_resolver::ExtractClassAttr(attr);
    const book_xml_css_style_utils::MarginTopResult mtr =
        book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
    book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
    const int line_h = ts->GetHeight() + ts->linespacing;
    const int default_lf =
        !book_xml_screen_advance::Blankline(p) ? 1 : 0;
    const int lf_count = book_xml_parser_style_utils::ResolveBlockTopLinefeeds(
        default_lf, mtr, line_h);
    LogResolvedBlockMargin(p, "hr", "top", p->last_hr_style,
                           p->last_hr_class, mtr, line_h, default_lf, lf_count);
    book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
        p, "hr", "hr-top", mtr, line_h, default_lf);
    if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
      book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, "hr");
      const int content_left =
          ts->margin.left + parse_current_block_margin_left(p);
      const int content_right =
          ts->display.width - ts->margin.right -
          parse_current_block_margin_right(p);
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

      const u32 x0_u = (u32)std::max(0, std::min(255, rule_x0));
      const u32 x1_u = (u32)std::max(x0_u, (u32)std::min(255, rule_x1));

      parse_append_page_byte(p, TEXT_HR_BOUNDS);
      parse_append_page_byte(p, x0_u);
      parse_append_page_byte(p, x1_u);
      p->current_screen_has_drawable_content = true;
      p->pen.y += ts->GetHeight() + ts->linespacing;
      p->pen.x = ts->margin.left;
      p->linebegan = false;
    }
  } else if (!strcmp(el, "pre")) {
    parse_push(p, TAG_PRE);
    p->preformatted_wrap_enabled = true;
    parse_append_page_byte(p, TEXT_PRE_ON);
    if (!p->mono) {
      parse_append_page_byte(p, TEXT_MONO_ON);
      p->mono = true;
      ts->SetStyle(book_xml_parser_style_utils::ResolveParsedTextStyle(
          p->bold, p->italic, p->mono));
    }
  } else if (!strcmp(el, "li")) {
    parse_push(p, TAG_LI);
    book_xml_list_utils::MarkCurrentListItemPending(p, true);
    if (book_xml_inline_state::HasActiveStackHiddenStyle(p)) {
      if (out_early_return) *out_early_return = true;
      return true;
    }
    const context_t active_list =
        book_xml_list_utils::GetActiveListContext(p);
    const int nested_indent =
        book_xml_list_utils::ResolveNestedListItemIndentPx(
            book_xml_list_utils::GetActiveListDepth(p), ts->GetAdvance(' '));
    if (nested_indent != 0) {
      parse_set_current_block_margins(
          p, parse_current_block_margin_left(p) + nested_indent,
          parse_current_block_margin_right(p));
    }
    const bool suppress_marker =
        book_xml_list_utils::HasSuppressedListMarkerContext(p) ||
        book_xml_list_utils::ParseListMarkerHiddenCssClass(p, attr);
    if (active_list == TAG_UL || active_list == TAG_OL) {
      if (p->linebegan && p->buflen > 0 && p->buf[p->buflen - 1] != '\n')
        book_xml_screen_advance::Linefeed(p);
      // Prevent orphan markers: if the marker's line is the last usable line
      // on the screen (chardata would immediately advance before the first
      // content character), push the marker to the next screen now.
      book_xml_screen_advance::AdvanceParsedPageOnOverflow(
          p, ts->GetHeight());
      AlignFreshLineToBlockMargin(p, ts);
      if (!suppress_marker) {
        if (active_list == TAG_UL) {
          parse_append_page_byte(p, 0x2022); // bullet '•'
          p->pen.x += ts->GetAdvance(0x2022) + ts->GetAdvance(' ');
        } else {
          const std::string marker = book_xml_list_utils::BuildOrderedListMarker(
              book_xml_list_utils::AdvanceOrderedListOrdinal(p),
              book_xml_list_utils::GetActiveOrderedListStyle(p));
          for (size_t i = 0; i < marker.size(); i++) {
            parse_append_page_byte(p, (u32)(unsigned char)marker[i]);
            p->pen.x += ts->GetAdvance((u32)(unsigned char)marker[i]);
          }
          p->pen.x += ts->GetAdvance(' ');
        }
        parse_append_page_byte(p, ' ');
        p->linebegan = true;
        p->strip_leading_list_marker = true;
      }
    }
  } else if (!strcmp(el, "script")) {
    parse_push(p, TAG_SCRIPT);
  } else if (!strcmp(el, "style")) {
    parse_push(p, TAG_STYLE);
  } else if (!strcmp(el, "ul")) {
    parse_push(p, TAG_UL);
    book_xml_list_utils::ConfigureElementListSemantics(p, attr);
  } else {
    return false;
  }

  return true;
}

bool HandleBlockElementEnd(parsedata_t *p, Text *ts, const char *el) {
  if (!p || !ts || !el)
    return false;

  const FlowEmissionFns fns = MakeLocalFlowEmissionFns();

  if (!strcmp(el, "br")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, "br");
    book_xml_screen_advance::Linefeed(p);
  } else if (!strcmp(el, "aside")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_aside_style, p->last_aside_class, p->css_class_map, "aside");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "aside", "aside-bottom", mbr, line_h, 2);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "blockquote")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_blockquote_style, p->last_blockquote_class,
              p->css_class_map, "blockquote");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "blockquote", "blockquote-bottom", mbr, line_h, 1);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "caption")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_caption_style, p->last_caption_class,
              p->css_class_map, "caption");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "caption", "caption-bottom", mbr, line_h, 1);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "dd")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_dd_style, p->last_dd_class, p->css_class_map, "dd");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "dd", "dd-bottom", mbr, line_h, 1);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "figure")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_figure_style, p->last_figure_class,
              p->css_class_map, "figure");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "figure", "figure-bottom", mbr, line_h, 1);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "p")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    if (p->paragraph_has_content &&
        !book_xml_list_utils::IsInsideListItem(p) &&
        !ParseInAnyEasyParagraphTightBlock(p)) {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_p_style, p->last_p_class, p->css_class_map, "p");
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "p", "bottom", p->last_p_style,
                             p->last_p_class, mbr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "p", "paragraph-bottom", mbr, line_h, default_lf);
      if (!p->pending_block_spacing_from_css && p->pending_block_spacing_lf < 1)
        p->pending_block_spacing_lf = 1;
#ifdef DSLIBRIS_DEBUG
      DBG_LOGF(p->book->GetStatusReporter(),
        "P-END cls=%s pbb=%d pbl=%d from_css=%d pen_y=%d lb=%d scr=%d",
        p->last_p_class.empty() ? "-" : p->last_p_class.c_str(),
        p->pending_block_break ? 1 : 0,
        p->pending_block_spacing_lf,
        p->pending_block_spacing_from_css ? 1 : 0,
        p->pen.y, p->linebegan ? 1 : 0, p->screen);
#endif
    }
    RestoreActiveBlockTextAlignMarker(p);
    p->in_paragraph = false;
    p->paragraph_has_content = false;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "div")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_div_style, p->last_div_class, p->css_class_map, "div");
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "div", "div-bottom", mbr, line_h, 0);
    }
    p->block_margin_left = 0;
    p->block_margin_right = 0;
    p->last_div_class.clear();
    p->last_div_style.clear();
  } else if (!strcmp(el, "h1")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_h1_style, p->last_h1_class, p->css_class_map, "h1");
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h1", "bottom", p->last_h1_style,
                             p->last_h1_class, mbr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "h1", "heading-bottom", mbr, line_h, default_lf);
    }
    RestoreHeadingFontSize(p, ts);
    RestoreActiveBlockTextAlignMarker(p);
    if (!Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "h2")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_h2_style, p->last_h2_class, p->css_class_map, "h2");
      const int default_lf = 1;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "h2", "bottom", p->last_h2_style,
                             p->last_h2_class, mbr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, "h2", "heading-bottom", mbr, line_h, default_lf);
    }
    RestoreHeadingFontSize(p, ts);
    RestoreActiveBlockTextAlignMarker(p);
    if (!Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "h3") || !strcmp(el, "h4") || !strcmp(el, "h5") ||
             !strcmp(el, "h6") || !strcmp(el, "hr")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    if (!strcmp(el, "hr")) {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_hr_style, p->last_hr_class, p->css_class_map, "hr");
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, "hr", "bottom", p->last_hr_style,
                             p->last_hr_class, mbr, line_h, default_lf, lf_count);
      if (ShouldRenderHrRule(p->last_hr_style, p->last_hr_class)) {
        // TEXT_HR_BOUNDS was emitted, so the renderer has linebegan=false.
        // Emit \n bytes for overflow check only — do NOT advance pen.y.
        for (int i = 0; i < lf_count; i++)
          parse_append_page_byte(p, '\n');
      } else {
        for (int i = 0; i < lf_count; i++)
          book_xml_screen_advance::Linefeed(p);
      }
    } else {
      const int line_h = ts->GetHeight() + ts->linespacing;
      const book_xml_css_style_utils::MarginTopResult mbr =
          book_xml_css_resolver::ParseElementMarginBottomWithClass(
              p->last_h_style, p->last_h_class, p->css_class_map, el);
      const int default_lf = 2;
      const int lf_count = book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
          default_lf, mbr, line_h);
      LogResolvedBlockMargin(p, el, "bottom", p->last_h_style,
                             p->last_h_class, mbr, line_h, default_lf, lf_count);
      book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
          p, el, "heading-bottom", mbr, line_h, default_lf);
      RestoreHeadingFontSize(p, ts);
    }
    if (strcmp(el, "hr"))
      RestoreActiveBlockTextAlignMarker(p);
    if (!strcmp(el, "h3") && !Trim(p->doc_heading).empty())
      p->doc_heading_complete = true;
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  } else if (!strcmp(el, "pre")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    parse_append_page_byte(p, TEXT_PRE_OFF);
    p->preformatted_wrap_enabled = false;
    book_xml_screen_advance::QueueBlockSpacingLines(
        p, 2, "pre", "block-bottom", false);
  } else if (!strcmp(el, "code") || !strcmp(el, "tt") ||
             !strcmp(el, "kbd") || !strcmp(el, "samp")) {
    // no-op: inline code elements have no end-side block action
  } else if (!strcmp(el, "li") || !strcmp(el, "ul") || !strcmp(el, "ol")) {
    book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, fns);
    if (!strcmp(el, "li"))
      p->strip_leading_list_marker = false;
    if (p->linebegan)
      book_xml_screen_advance::QueueBlockSpacingLines(
          p, 1, el, "list-item-bottom", false);
  } else {
    return false;
  }

  return true;
}

void ApplyDisplayBlockPromotion(
    parsedata_t *p, Text *ts, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  if (!p || !ts || !el)
    return;
  if (IsBlockLevelElement(el))
    return;
  if (p->in_paragraph)
    return;
  if (!elem_css.is_display_block)
    return;
  if (!book_xml_screen_advance::Blankline(p))
    book_xml_screen_advance::Linefeed(p);
  const book_xml_css_style_utils::MarginTopResult mtr =
      book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
  book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
  const int line_h = ts->GetHeight() + ts->linespacing;
  const int default_lf = 1;
  book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
      p, el, "block-top", mtr, line_h, default_lf);
}

} // namespace book_xml_block_handler
