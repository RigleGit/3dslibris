/*
    3dslibris - book_xml_heading_handler.cpp

    Heading element handler extracted from book_xml_parser.cpp.

    Handles h1–h3 start events with keep-with-next logic, font-size
    application, and block spacing queuing. ApplyHeadingFontSize and
    RestoreHeadingFontSize are also called from book_xml_parser.cpp for h4–h6.
    ShouldRenderHrRule is called from the hr start/end handlers.
*/

#include "book/book_xml_heading_handler.h"
#include "book/book.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/epub_css_class_map.h"
#include "book/heading_layout.h"
#include "parse.h"
#include "shared/text_render_layout_utils.h"
#include "ui/text.h"

#include <algorithm>
#include <cctype>
#include <string.h>
#include <string>

namespace {

static std::string ToLowerLocal(const std::string &s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); i++)
    out[i] = (char)tolower((unsigned char)out[i]);
  return out;
}

static bool ContainsAsciiNoCase(const std::string &haystack,
                                const char *needle) {
  if (!needle || !needle[0])
    return false;
  return ToLowerLocal(haystack).find(ToLowerLocal(needle)) != std::string::npos;
}

static bool EqualsAsciiNoCaseLocal(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCaseLocal(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return colon && (strcmp(colon + 1, needle) == 0 ||
                   EqualsAsciiNoCaseLocal(colon + 1, needle));
}

static void AppendAlignMarker(parsedata_t *p,
                              book_xml_css_style_utils::TextAlign align) {
  using TA = book_xml_css_style_utils::TextAlign;
  if (!p)
    return;
  if (align == TA::Center)
    parse_append_page_byte(p, TEXT_PARAGRAPH_CENTER);
  else if (align == TA::Right)
    parse_append_page_byte(p, TEXT_PARAGRAPH_RIGHT);
  else
    parse_append_page_byte(p, TEXT_PARAGRAPH_LEFT);
}

static book_xml_css_style_utils::MarginTopResult
ParseMarginTopLocal(const char **attr,
                    const epub_css_class_map::CssClassMargins &elem_css) {
  const book_xml_css_style_utils::MarginTopResult from_style =
      book_xml_css_resolver::ParseElementMarginTopPx(attr);
  if (from_style.unit != book_xml_css_style_utils::MarginTopResult::Unit::None)
    return from_style;
  return elem_css.margin_top;
}

static void ApplyBlockMarginsLocal(
    parsedata_t *p, Text *ts, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::MarginTopResult;
  using book_xml_css_style_utils::ResolveHorizontalMarginPx;
  if (!p || !ts)
    return;
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
    effective_left += ResolveHorizontalMarginPx(ml, ts->display.width, (int)ts->GetPixelSize());
  if (mr.unit != MarginTopResult::Unit::None)
    effective_right += ResolveHorizontalMarginPx(mr, ts->display.width, (int)ts->GetPixelSize());
  parse_set_current_block_margins(p, effective_left, effective_right);
}

static int ResolveHeadingFontSizePx(parsedata_t *p, Text *ts, int heading_level,
                                    const std::string &style_attr,
                                    const std::string &class_attr) {
  if (!p || !ts)
    return 0;
  const int inherited_px = (int)ts->GetPixelSize();
  const int default_heading_base =
      (p->base_font_size_px != 0) ? (int)p->base_font_size_px : inherited_px;
  return book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
      inherited_px, default_heading_base, heading_level, style_attr, class_attr,
      p->css_class_map, (int)p->css_px_baseline);
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

} // namespace

void ApplyHeadingFontSize(parsedata_t *p, Text *ts, int heading_level,
                          const std::string &style_attr,
                          const std::string &class_attr) {
  if (!p || !ts || p->stacksize == 0)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  const int inherited_px = (int)ts->GetPixelSize();
  const int default_heading_base =
      (p->base_font_size_px != 0) ? (int)p->base_font_size_px : inherited_px;
  p->heading_saved_font_size_stack[current] = (u8)inherited_px;
  p->heading_font_size_emitted_stack[current] = false;

  const int heading_px = book_xml_parser_style_utils::ComputeHeadingFontSizeForContext(
      inherited_px, default_heading_base, heading_level,
      style_attr, class_attr, p->css_class_map, (int)p->css_px_baseline);
  if (heading_px == inherited_px)
    return;

  ts->SetPixelSize((u8)heading_px);
  parse_append_page_byte(p, TEXT_FONT_SIZE);
  parse_append_page_byte(p, (u32)heading_px);
  p->heading_font_size_emitted_stack[current] = true;
}

void RestoreHeadingFontSize(parsedata_t *p, Text *ts) {
  if (!p || !ts || p->stacksize == 0)
    return;

  const u8 current = (u8)(p->stacksize - 1);
  if (!p->heading_font_size_emitted_stack[current])
    return;

  ts->SetPixelSize(p->heading_saved_font_size_stack[current]);
  parse_append_page_byte(p, TEXT_FONT_SIZE);
  parse_append_page_byte(p, p->heading_saved_font_size_stack[current]);
  p->heading_font_size_emitted_stack[current] = false;
}

bool ShouldRenderHrRule(const std::string &style_attr,
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

void HandleHeadingStart(parsedata_t *p, Text *ts, const char **attr,
                        const epub_css_class_map::CssClassMargins &elem_css,
                        int heading_level, const HeadingHandlerFns &fns) {
  const std::string heading_style = book_xml_css_resolver::ExtractStyleAttr(attr);
  const std::string heading_class = book_xml_css_resolver::ExtractClassAttr(attr);

  fns.ensure_block_boundary(p, "heading", "heading-block-boundary");

  const int heading_px =
      ResolveHeadingFontSizePx(p, ts, heading_level, heading_style, heading_class);

  heading_layout::KeepWithNextRequest req{};
  {
    const int pending_lf = p->pending_block_spacing_lf;
    const int lh_step = ts->GetHeight() + std::max(0, ts->linespacing);
    req.pen_y = p->pen.y + (pending_lf > 0 ? pending_lf * lh_step : 0);
  }
  const text_render_layout_utils::ReadingScreenMetrics metrics =
      text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
          p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
          text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
  req.screen_height = metrics.max_height;
  req.bottom_margin = metrics.bottom_margin;
  req.line_height = MeasureLineHeightForPixelSize(ts, heading_px);
  req.linespacing = ts->linespacing;
  req.heading_level = heading_level;
  if (heading_layout::ShouldAdvanceHeadingForKeepWithNext(req))
    fns.advance_screen(p);

  const char *tag_name = "h1";
  if (heading_level == 1) {
    parse_push(p, TAG_H1);
    p->last_h1_style = heading_style;
    p->last_h1_class = heading_class;
    ApplyHeadingFontSize(p, ts, 1, p->last_h1_style, p->last_h1_class);
    AppendAlignMarker(p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
                             p->last_h1_style, p->last_h1_class,
                             p->block_text_align_stack,
                             p->block_text_align_value_stack, p->stacksize,
                             p->css_class_map, "h1"));
    tag_name = "h1";
  } else if (heading_level == 2) {
    parse_push(p, TAG_H2);
    p->last_h2_style = heading_style;
    p->last_h2_class = heading_class;
    ApplyHeadingFontSize(p, ts, 2, p->last_h2_style, p->last_h2_class);
    AppendAlignMarker(p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
                             p->last_h2_style, p->last_h2_class,
                             p->block_text_align_stack,
                             p->block_text_align_value_stack, p->stacksize,
                             p->css_class_map, "h2"));
    tag_name = "h2";
  } else {
    parse_push(p, TAG_H3);
    p->last_h_style = heading_style;
    p->last_h_class = heading_class;
    ApplyHeadingFontSize(p, ts, 3, p->last_h_style, p->last_h_class);
    AppendAlignMarker(p, book_xml_css_resolver::ResolveElementTextAlignWithClass(
                             p->last_h_style, p->last_h_class,
                             p->block_text_align_stack,
                             p->block_text_align_value_stack, p->stacksize,
                             p->css_class_map, "h3"));
    tag_name = "h3";
  }

  parse_append_page_byte(p, TEXT_BOLD_ON);
  p->pos++;
  p->bold = true;

  const book_xml_css_style_utils::MarginTopResult mtr =
      ParseMarginTopLocal(attr, elem_css);
  ApplyBlockMarginsLocal(p, ts, attr, elem_css);
  const int line_h = ts->GetHeight() + ts->linespacing;
  const int default_lf = 2;
  fns.queue_block_spacing(p, tag_name, "heading-top", mtr, line_h, default_lf);
  if (p->pending_block_spacing_lf < 1)
    p->pending_block_spacing_lf = 1;
}
