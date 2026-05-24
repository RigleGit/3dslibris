/*
    3dslibris - book_xml_image_handler.cpp

    Inline image handler extracted from book_xml_parser.cpp.

    Processes <img>/<image> start events: attr extraction, path resolution,
    inline image layout planning, and token emission into the page buffer.
*/

#include "book/book_xml_image_handler.h"
#include "book/book.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/epub_css_class_map.h"
#include "book/inline_image_layout.h"
#include "formats/common/epub_image_utils.h"
#include "parse.h"
#include "ui/text.h"

#include <algorithm>
#include <string.h>
#include <string>
#include <vector>

namespace {

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

static bool XmlNameEqualsLocal(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCaseLocal(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return colon && (strcmp(colon + 1, needle) == 0 ||
                   EqualsAsciiNoCaseLocal(colon + 1, needle));
}

static bool AttrNameEqualsLocal(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCaseLocal(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return colon && (strcmp(colon + 1, needle) == 0 ||
                   EqualsAsciiNoCaseLocal(colon + 1, needle));
}

static bool BlankLineLocal(const parsedata_t *p) {
  if (!p || p->buflen < 3)
    return true;
  return (p->buf[p->buflen - 1] == '\n') && (p->buf[p->buflen - 2] == '\n');
}

static book_xml_css_style_utils::ClearMode
ParseElementClearLocal(const char **attr,
                       const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::ClearMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEqualsLocal(attr[i], "style")) {
        ClearMode mode = ClearMode::None;
        if (book_xml_css_style_utils::TryParseClear(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_clear ? elem_css.clear_mode : ClearMode::None;
}

static book_xml_css_style_utils::FloatMode
ParseElementFloatLocal(const char **attr,
                       const epub_css_class_map::CssClassMargins &elem_css) {
  using book_xml_css_style_utils::FloatMode;
  if (attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (AttrNameEqualsLocal(attr[i], "style")) {
        FloatMode mode = FloatMode::None;
        if (book_xml_css_style_utils::TryParseFloat(attr[i + 1], &mode))
          return mode;
      }
    }
  }
  return elem_css.has_float ? elem_css.float_mode : FloatMode::None;
}

static std::string NormalizeDocPathLocal(const std::string &path) {
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

static std::string ResolveDocPathLocal(const std::string &base_doc_path,
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
    return NormalizeDocPathLocal(clean_href);

  std::string base = base_doc_path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos)
    base = base.substr(0, slash + 1);
  else
    base.clear();

  return NormalizeDocPathLocal(base + clean_href);
}

} // namespace

void HandleInlineImageStart(parsedata_t *p, Text *ts, const char **attr,
                            const epub_css_class_map::CssClassMargins &elem_css,
                            const ImageHandlerFns &fns) {
  parse_push(p, TAG_UNKNOWN);

  const char *src = NULL;
  const char *img_style = NULL;
  const char *img_width_attr = NULL;
  for (int i = 0; attr && attr[i]; i += 2) {
    if (XmlNameEqualsLocal(attr[i], "src") || XmlNameEqualsLocal(attr[i], "href"))
      src = attr[i + 1];
    else if (AttrNameEqualsLocal(attr[i], "style"))
      img_style = attr[i + 1];
    else if (AttrNameEqualsLocal(attr[i], "width"))
      img_width_attr = attr[i + 1];
  }

  const book_xml_css_style_utils::FloatMode float_mode =
      ParseElementFloatLocal(attr, elem_css);
  if (ParseElementClearLocal(attr, elem_css) !=
      book_xml_css_style_utils::ClearMode::None) {
    if (!BlankLineLocal(p) && p->linebegan)
      fns.linefeed(p);
  }

  if (img_style) {
    const int line_h = ts->GetHeight() + ts->linespacing;
    const book_xml_css_style_utils::MarginTopResult mtr =
        book_xml_css_style_utils::ParseMarginTop(img_style);
    const int default_lf = !BlankLineLocal(p) ? 1 : 0;
    const int lf_count =
        book_xml_parser_style_utils::ResolveBlockTopLinefeeds(default_lf,
                                                              mtr, line_h);
    for (int i = 0; i < lf_count; i++)
      fns.linefeed(p);
  }

  std::string resolved;
  if (src && *src) {
    std::string raw_src(src);
    if (!raw_src.empty() && raw_src[0] == '#') {
      resolved = "fb2:" + raw_src.substr(1);
    } else {
      resolved = ResolveDocPathLocal(p->docpath, raw_src);
    }
  }

  if (!resolved.empty() && p->book) {
    u16 image_id = p->book->RegisterInlineImage(resolved);
    const int text_w = ts->display.width - ts->margin.left - ts->margin.right;
    const int root_font_px =
        (p->base_font_size_px != 0) ? (int)p->base_font_size_px
                                    : ts->GetPixelSize();
    const int author_max_w =
        book_xml_css_resolver::ParseImgWidthPx(img_width_attr, img_style,
                                               text_w, ts->GetHeight(),
                                               root_font_px);
    if (author_max_w > 0)
      p->book->SetInlineImageAuthorMaxWidth(image_id, author_max_w);
    InlineImageLayoutPlan image_plan{};
    const bool leading_paragraph_image =
        p->in_paragraph && !p->paragraph_has_content;

    const bool figure_with_caption = parse_in(p, TAG_FIGURE);

    const InlineImageContext image_context =
        figure_with_caption
            ? INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION
            : (leading_paragraph_image
                  ? INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH
                  : INLINE_IMAGE_CONTEXT_DEFAULT);
    p->book->PlanInlineImageLayout(ts, image_id, p->screen, p->pen.x,
                                   p->pen.y, p->linebegan,
                                   image_context, &image_plan);

    if (float_mode != book_xml_css_style_utils::FloatMode::None)
      ApplyFloatImageLayoutOverride(&image_plan, p->linebegan,
                                    ts->linespacing);

    InlineImageMetadata img_meta{};
    p->book->GetInlineImageMetadata(image_id, &img_meta);
    if (!img_meta.ok && image_plan.mode == INLINE_IMAGE_LAYOUT_PAGE &&
        epub_image_utils::LooksLikeSvgWrapper(resolved,
                                              std::vector<u8>())) {
      const char *fallback = "[illustration]";
      if (!BlankLineLocal(p))
        fns.linefeed(p);
      fns.emit_chardata(p, fallback, (int)strlen(fallback));
      fns.linefeed(p);
      return;
    }

    if (image_plan.advance_before)
      fns.advance_screen(p);
    if (image_plan.line_break_before && p->linebegan)
      fns.linefeed(p);

    if (figure_with_caption) {
      parse_append_page_byte(p, TEXT_IMAGE_FIGURE_WITH_CAPTION);
    } else if (leading_paragraph_image) {
      parse_append_page_byte(p, TEXT_IMAGE_LEADING_PARAGRAPH);
    } else {
      parse_append_page_byte(p, TEXT_IMAGE_CONTEXT_DEFAULT);
    }
    if (author_max_w > 0) {
      parse_append_page_byte(p, TEXT_IMAGE_AUTHOR_WIDTH);
      parse_append_page_byte(p, (u32)author_max_w);
    }
    if (float_mode == book_xml_css_style_utils::FloatMode::Left) {
      parse_append_page_byte(p, TEXT_IMAGE_ALIGN);
      parse_append_page_byte(p, 1);
    } else if (float_mode == book_xml_css_style_utils::FloatMode::Right) {
      parse_append_page_byte(p, TEXT_IMAGE_ALIGN);
      parse_append_page_byte(p, 2);
    }
    parse_append_page_byte(p, TEXT_IMAGE);
    parse_append_page_byte(p, (u32)image_id);

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
      p->pen.x = ts->margin.left;
      p->pen.y += image_plan.vertical_space_after_draw;
      p->linebegan = false;
      fns.advance_page_overflow(p, ts->GetHeight());
      if (img_style) {
        const int line_h = ts->GetHeight() + ts->linespacing;
        const book_xml_css_style_utils::MarginTopResult mbr =
            book_xml_css_style_utils::ParseMarginBottom(img_style);
        const int lf_count =
            book_xml_parser_style_utils::ResolveBlockBottomLinefeeds(
                0, mbr, line_h);
        for (int i = 0; i < lf_count; i++)
          fns.linefeed(p);
      }
      break;

    case INLINE_IMAGE_LAYOUT_PAGE:
    default:
      if (p->in_paragraph)
        p->paragraph_has_content = true;
      fns.advance_screen(p);
      break;
    }
  } else {
    const char *fallback = "[illustration]";
    if (!BlankLineLocal(p))
      fns.linefeed(p);
    fns.emit_chardata(p, fallback, (int)strlen(fallback));
    fns.linefeed(p);
  }
}
