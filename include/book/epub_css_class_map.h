#pragma once

#include "book/book_xml_css_style_utils.h"

#include <map>
#include <string>

namespace epub_css_class_map {

using book_xml_css_style_utils::MarginTopResult;
using book_xml_css_style_utils::TextAlign;
using book_xml_css_style_utils::FontSizeSpec;

struct CssClassMargins {
  MarginTopResult margin_top;
  MarginTopResult margin_bottom;
  FontSizeSpec font_size;
  bool hide_list_markers;
  bool has_text_align;
  TextAlign text_align;
  bool superscript;
  bool subscript;

  CssClassMargins()
      : hide_list_markers(false), has_text_align(false),
        text_align(TextAlign::Left), superscript(false), subscript(false) {}
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

} // namespace epub_css_class_map
