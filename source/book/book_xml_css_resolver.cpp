#include "book/book_xml_css_resolver.h"

#include "book/book_xml_css_style_utils.h"
#include "book/epub_css_class_map.h"
#include "shared/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

static bool AttrNameEq(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle))
    return true;
  const char ca = name[0] >= 'A' && name[0] <= 'Z'
                      ? (char)(name[0] - 'A' + 'a')
                      : name[0];
  const char cn = needle[0] >= 'A' && needle[0] <= 'Z'
                      ? (char)(needle[0] - 'A' + 'a')
                      : needle[0];
  if (ca != cn)
    return false;
  const std::string name_lc = ToLowerAscii(std::string(name));
  if (name_lc == needle)
    return true;
  const char *colon = strrchr(name, ':');
  if (!colon)
    return false;
  const std::string local = ToLowerAscii(std::string(colon + 1));
  return local == needle;
}

static bool HasClassToken(const char *class_name, const char *token) {
  if (!class_name || !token || !token[0])
    return false;
  const std::string class_lc = ToLowerAscii(std::string(class_name));
  const std::string token_lc = ToLowerAscii(std::string(token));
  return ContainsToken(class_lc, token_lc);
}

static std::string RemoveAsciiWhitespace(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); i++) {
    const char c = value[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
      out.push_back(c);
  }
  return out;
}

static void ParseClassStyleFlagsInternal(
    const char *class_name, bool *bold_out, bool *italic_out,
    bool *underline_out, uint8_t *underline_style_out, bool *overline_out,
    bool *strikethrough_out, bool *superscript_out, bool *subscript_out) {
  if (!class_name)
    return;
  const std::string class_lc = ToLowerAscii(std::string(class_name));

  if (italic_out) {
    if (ContainsNoCase(class_lc, "italic") ||
        ContainsNoCase(class_lc, "oblique") ||
        ContainsNoCase(class_lc, "emphasis"))
      *italic_out = true;
  }
  if (bold_out) {
    if (ContainsNoCase(class_lc, "bold") ||
        ContainsNoCase(class_lc, "semibold") ||
        ContainsNoCase(class_lc, "font-weight"))
      *bold_out = true;
  }
  if (underline_out) {
    if (ContainsNoCase(class_lc, "underline") ||
        ContainsNoCase(class_lc, "underlined")) {
      *underline_out = true;
      if (underline_style_out) {
        if (ContainsNoCase(class_lc, "wavy"))
          *underline_style_out = UNDERLINE_STYLE_WAVY;
        else if (ContainsNoCase(class_lc, "dashed"))
          *underline_style_out = UNDERLINE_STYLE_DASHED;
        else if (ContainsNoCase(class_lc, "dotted"))
          *underline_style_out = UNDERLINE_STYLE_DOTTED;
      }
    }
  }
  if (overline_out) {
    if (ContainsNoCase(class_lc, "overline") ||
        ContainsNoCase(class_lc, "overlined"))
      *overline_out = true;
  }
  if (strikethrough_out) {
    if (ContainsNoCase(class_lc, "strikethrough") ||
        ContainsNoCase(class_lc, "line-through") ||
        ContainsNoCase(class_lc, "strike") ||
        ContainsNoCase(class_lc, "deleted"))
      *strikethrough_out = true;
  }
  if (superscript_out) {
    if (ContainsNoCase(class_lc, "superscript"))
      *superscript_out = true;
  }
  if (subscript_out) {
    if (ContainsNoCase(class_lc, "subscript"))
      *subscript_out = true;
  }
}

static book_xml_css_style_utils::MarginTopResult
LookupClassMarginBottomInternal(
    const std::string &class_attr,
    const epub_css_class_map::CssClassMap &class_map) {
  epub_css_class_map::CssClassMargins margins;
  if (epub_css_class_map::LookupMarginsForClassAttr(class_attr, class_map,
                                                     &margins))
    return margins.margin_bottom;
  return {};
}

static bool FindActiveBlockTextAlignInternal(
    const bool *block_text_align_stack,
    const uint8_t *block_text_align_value_stack, uint8_t stacksize,
    book_xml_css_style_utils::TextAlign *out) {
  if (!block_text_align_stack || !block_text_align_value_stack || !out)
    return false;
  for (int i = (int)stacksize - 1; i >= 0; --i) {
    if (!block_text_align_stack[i])
      continue;
    *out =
        (book_xml_css_style_utils::TextAlign)block_text_align_value_stack[i];
    return true;
  }
  return false;
}

} // namespace

namespace book_xml_css_resolver {

std::string ExtractStyleAttr(const char **attr) {
  if (!attr)
    return {};
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEq(attr[i], "style"))
      return std::string(attr[i + 1]);
  }
  return {};
}

std::string ExtractClassAttr(const char **attr) {
  if (!attr)
    return {};
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEq(attr[i], "class"))
      return std::string(attr[i + 1]);
  }
  return {};
}

int ParseCssLengthPx(const char *v, int text_width, int font_px) {
  if (!v || !*v)
    return 0;
  while (*v == ' ')
    v++;
  if (!*v)
    return 0;

  int num1000 = 0;
  bool has_digit = false;
  while (*v >= '0' && *v <= '9') {
    num1000 = num1000 * 10 + (*v - '0') * 1000;
    has_digit = true;
    v++;
    if (num1000 > 100000 * 1000)
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
      v++;
  }
  if (!has_digit)
    return 0;
  while (*v == ' ')
    v++;

  int result_px = 0;
  if (*v == '%') {
    if (num1000 <= 0 || num1000 > 100 * 1000)
      return 0;
    result_px = (text_width * num1000 + 50000) / 100000;
  } else if (*v == '\0') {
    result_px = (num1000 + 500) / 1000;
  } else if (v[0] == 'p' && v[1] == 'x') {
    result_px = (num1000 + 500) / 1000;
  } else if ((v[0] == 'e' && v[1] == 'm') ||
             (v[0] == 'r' && v[1] == 'e' && v[2] == 'm')) {
    result_px = (num1000 * font_px + 500) / 1000;
  } else if (v[0] == 'p' && v[1] == 't') {
    result_px = (num1000 * 4 + 1500) / 3000;
  } else if (v[0] == 'c' && v[1] == 'm') {
    result_px = (num1000 * 378 + 5000) / 10000;
  } else if (v[0] == 'm' && v[1] == 'm') {
    result_px = (num1000 * 378 + 50000) / 100000;
  } else if (v[0] == 'i' && v[1] == 'n') {
    result_px = (num1000 * 96 + 500) / 1000;
  } else if (v[0] == 'v' && v[1] == 'w') {
    if (num1000 <= 0 || num1000 > 100 * 1000)
      return 0;
    result_px = (240 * num1000 + 50000) / 100000;
  } else {
    return 0;
  }

  return std::max(0, std::min(result_px, text_width));
}

int ParseImgWidthPx(const char *width_attr, const char *style, int text_width,
                    int font_px) {
  return ParseImgWidthPx(width_attr, style, text_width, font_px, font_px);
}

int ParseImgWidthPx(const char *width_attr, const char *style, int text_width,
                    int font_px, int root_font_px) {
  const int image_font_px = root_font_px > 0 ? root_font_px : font_px;
  if (width_attr && *width_attr) {
    int px = ParseCssLengthPx(width_attr, text_width, image_font_px);
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
      int px = ParseCssLengthPx(lc.c_str() + pos, text_width, image_font_px);
      if (px > 0)
        return px;
    }
  }
  return 0;
}

void ParseElementStyleFlags(const char **attr, bool *bold_out,
                            bool *italic_out, bool *underline_out,
                            uint8_t *underline_style_out, bool *overline_out,
                            bool *strikethrough_out, bool *superscript_out,
                            bool *subscript_out, bool *no_underline_out,
                            bool *reset_bold_out, bool *reset_italic_out) {
  if ((!bold_out && !italic_out && !underline_out && !overline_out &&
       !strikethrough_out && !superscript_out && !subscript_out) ||
      !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEq(attr[i], "style")) {
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
    } else if (AttrNameEq(attr[i], "class")) {
      ParseClassStyleFlagsInternal(attr[i + 1], bold_out, italic_out,
                                   underline_out, underline_style_out,
                                   overline_out, strikethrough_out,
                                   superscript_out, subscript_out);
    }
  }
}

void ParseInlineHiddenFlags(const char *style, bool *hidden_out) {
  if (!style || !hidden_out)
    return;
  const std::string style_lc = ToLowerAscii(std::string(style));
  const std::string compact = RemoveAsciiWhitespace(style_lc);

  if (ContainsNoCase(compact, "display:none") ||
      ContainsNoCase(compact, "visibility:hidden") ||
      ContainsNoCase(compact, "clip:rect(0,0,0,0)") ||
      ContainsNoCase(compact, "clip:rect(1px,1px,1px,1px)") ||
      ContainsNoCase(compact, "clip:rect(0000)") ||
      ContainsNoCase(compact, "clip-path:inset(50%)") ||
      ContainsNoCase(compact, "clip-path:inset(100%)")) {
    *hidden_out = true;
    return;
  }

  const bool tiny =
      ContainsNoCase(compact, "width:1px") &&
      ContainsNoCase(compact, "height:1px");
  const bool offscreen = ContainsNoCase(compact, "position:absolute");
  const bool hidden_overflow = ContainsNoCase(compact, "overflow:hidden");
  const bool clipped = ContainsNoCase(compact, "clip:rect(") ||
                       ContainsNoCase(compact, "clip-path:inset(");
  if (tiny && offscreen && (hidden_overflow || clipped))
    *hidden_out = true;
}

void ParseClassHiddenFlags(const char *class_name, bool *hidden_out) {
  if (!class_name || !hidden_out)
    return;
  if (HasClassToken(class_name, "visually-hidden") ||
      HasClassToken(class_name, "visuallyhidden") ||
      HasClassToken(class_name, "sr-only") ||
      HasClassToken(class_name, "screen-reader-text"))
    *hidden_out = true;
}

void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  if (!hidden_out || !attr)
    return;
  for (int i = 0; attr[i]; i += 2) {
    const char *name = attr[i];
    const char *value = attr[i + 1];
    if (AttrNameEq(name, "hidden")) {
      *hidden_out = true;
    } else if (AttrNameEq(name, "aria-hidden")) {
      if (!value || !value[0] || !strcmp(value, "1") ||
          !strcmp(value, "true") || !strcmp(value, "yes") ||
          !strcmp(value, "hidden"))
        *hidden_out = true;
    } else if (value && value[0] && AttrNameEq(name, "style")) {
      ParseInlineHiddenFlags(value, hidden_out);
    } else if (value && value[0] && AttrNameEq(name, "class")) {
      ParseClassHiddenFlags(value, hidden_out);
    }
  }
}

bool StyleLooksDisplayBlock(const std::string &style_attr) {
  const std::string lc = ToLowerAscii(style_attr);
  return ContainsNoCase(lc, "display:block") ||
         ContainsNoCase(lc, "display: block");
}

bool ElementCanCarryBlockTextAlign(const char *el,
                                   const std::string &style_attr) {
  return !strcmp(el, "body") || !strcmp(el, "div") ||
         !strcmp(el, "p") || !strcmp(el, "h1") || !strcmp(el, "h2") ||
         !strcmp(el, "h3") || !strcmp(el, "h4") || !strcmp(el, "h5") ||
         !strcmp(el, "h6") || !strcmp(el, "article") ||
         !strcmp(el, "aside") || !strcmp(el, "blockquote") ||
         !strcmp(el, "caption") || !strcmp(el, "figure") ||
         !strcmp(el, "section") || !strcmp(el, "header") ||
         !strcmp(el, "footer") || !strcmp(el, "dl") ||
         !strcmp(el, "dt") || !strcmp(el, "dd") ||
         StyleLooksDisplayBlock(style_attr);
}

book_xml_css_style_utils::TextAlign ResolveElementTextAlignWithClass(
    const std::string &style_attr, const std::string &class_attr,
    const bool *block_text_align_stack, const uint8_t *block_text_align_value_stack,
    uint8_t stacksize, const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag) {
  book_xml_css_style_utils::TextAlign align =
      book_xml_css_style_utils::TextAlign::Left;
  if (book_xml_css_style_utils::TryParseTextAlign(style_attr.c_str(), &align))
    return align;
  if (epub_css_class_map::LookupTextAlignForClassAttr(class_attr, class_map,
                                                      &align))
    return align;
  if (element_tag &&
      epub_css_class_map::LookupTextAlignForTag(element_tag, class_map, &align))
    return align;
  if (FindActiveBlockTextAlignInternal(block_text_align_stack,
                                       block_text_align_value_stack, stacksize,
                                       &align))
    return align;
  return book_xml_css_style_utils::TextAlign::Left;
}

book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopPx(const char **attr) {
  book_xml_css_style_utils::MarginTopResult empty;
  if (!attr)
    return empty;
  for (int i = 0; attr[i]; i += 2) {
    if (!attr[i + 1] || !attr[i + 1][0])
      continue;
    if (AttrNameEq(attr[i], "style"))
      return book_xml_css_style_utils::ParseMarginTop(attr[i + 1]);
  }
  return empty;
}

book_xml_css_style_utils::MarginTopResult ParseElementMarginBottomWithClass(
    const std::string &last_style, const std::string &last_class,
    const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag) {
  using book_xml_css_style_utils::MarginTopResult;
  const MarginTopResult from_style =
      book_xml_css_style_utils::ParseMarginBottom(last_style.c_str());
  if (from_style.unit != MarginTopResult::Unit::None)
    return from_style;
  const MarginTopResult from_class =
      LookupClassMarginBottomInternal(last_class, class_map);
  if (from_class.unit != MarginTopResult::Unit::None)
    return from_class;
  if (element_tag && element_tag[0]) {
    epub_css_class_map::CssClassMargins tag_css;
    if (epub_css_class_map::LookupAllForTag(element_tag, class_map, &tag_css))
      return tag_css.margin_bottom;
  }
  return {};
}

} // namespace book_xml_css_resolver
