#pragma once

#include "book/book_xml_css_style_utils.h"

#include <map>
#include <string>

namespace epub_css_class_map {

using book_xml_css_style_utils::MarginTopResult;
using book_xml_css_style_utils::TextAlign;
using book_xml_css_style_utils::FontSizeSpec;
using book_xml_css_style_utils::TextTransform;
using book_xml_css_style_utils::WhiteSpaceMode;
using book_xml_css_style_utils::FloatMode;
using book_xml_css_style_utils::ClearMode;

struct CssClassMargins {
  MarginTopResult margin_top;
  MarginTopResult margin_bottom;
  MarginTopResult margin_left;
  MarginTopResult margin_right;
  FontSizeSpec font_size;
  bool hide_list_markers;
  bool has_text_align;
  TextAlign text_align;
  bool has_white_space;
  WhiteSpaceMode white_space;
  bool superscript;
  bool subscript;
  bool page_break_before;
  bool page_break_after;
  bool page_break_inside_avoid;
  bool has_float;
  FloatMode float_mode;
  bool has_clear;
  ClearMode clear_mode;
  bool no_underline;   // text-decoration: none
  bool reset_bold;     // font-weight: normal/lighter/100-500
  bool reset_italic;   // font-style: normal
  MarginTopResult text_indent;
  bool has_text_transform;
  TextTransform text_transform;

  CssClassMargins()
      : hide_list_markers(false), has_text_align(false),
        text_align(TextAlign::Left), has_white_space(false),
        white_space(WhiteSpaceMode::Normal), superscript(false), subscript(false),
        page_break_before(false), page_break_after(false),
        page_break_inside_avoid(false), has_float(false),
        float_mode(FloatMode::None), has_clear(false),
        clear_mode(ClearMode::None),
        no_underline(false), reset_bold(false), reset_italic(false),
        has_text_transform(false), text_transform(TextTransform::None) {}
};

// Map: bare class name (no '.') → extracted margins.
using CssClassMap = std::map<std::string, CssClassMargins>;

// Parses raw CSS text; populates *out with class selectors that have
// margin-top/bottom rules. Other selectors and properties are skipped.
// Safe with css_text==nullptr or len==0.
void ParseCssIntoClassMap(const char *css_text, size_t len, CssClassMap *out);

// Resolves a raw HTML class="" attribute against the parsed CSS class map.
// Multiple whitespace-separated classes are supported. If several known
// classes match, their top/bottom margins are merged into *out.
bool LookupMarginsForClassAttr(const std::string &class_attr,
                               const CssClassMap &class_map,
                               CssClassMargins *out);

MarginTopResult LookupMarginLeftForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map);

MarginTopResult LookupMarginRightForClassAttr(const std::string &class_attr,
                                              const CssClassMap &class_map);

bool LookupHideListMarkersForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map);

bool LookupTextAlignForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map,
                                 TextAlign *out);

bool LookupFontSizeForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                FontSizeSpec *out);

// Returns true if any class in class_attr has a vertical-align:super/sub rule.
// superscript_out and subscript_out are OR-accumulated (never cleared).
bool LookupSuperSubForClassAttr(const std::string &class_attr,
                                const CssClassMap &class_map,
                                bool *superscript_out, bool *subscript_out);

bool LookupPageBreakBeforeForClassAttr(const std::string &class_attr,
                                       const CssClassMap &class_map);

bool LookupPageBreakAfterForClassAttr(const std::string &class_attr,
                                      const CssClassMap &class_map);
bool LookupPageBreakInsideAvoidForClassAttr(const std::string &class_attr,
                                            const CssClassMap &class_map);
bool LookupFloatForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             FloatMode *out);
bool LookupClearForClassAttr(const std::string &class_attr,
                             const CssClassMap &class_map,
                             ClearMode *out);

bool LookupNoUnderlineForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map);

bool LookupResetBoldForClassAttr(const std::string &class_attr,
                                 const CssClassMap &class_map);

bool LookupResetItalicForClassAttr(const std::string &class_attr,
                                   const CssClassMap &class_map);

// Returns Unit::None if no matching class has text-indent.
MarginTopResult LookupTextIndentForClassAttr(const std::string &class_attr,
                                             const CssClassMap &class_map);

// Returns false if no matching class specifies text-transform.
bool LookupTextTransformForClassAttr(const std::string &class_attr,
                                     const CssClassMap &class_map,
                                     TextTransform *out);

bool LookupWhiteSpaceForClassAttr(const std::string &class_attr,
                                  const CssClassMap &class_map,
                                  WhiteSpaceMode *out);

// Single-pass lookup of ALL CSS properties for a class="" attribute.
// Fills *out with merged values from all matching classes. Returns true if any
// class matched. Use this instead of calling individual Lookup* functions.
bool LookupAllForClassAttr(const std::string &class_attr,
                           const CssClassMap &class_map,
                           CssClassMargins *out);

} // namespace epub_css_class_map
