#pragma once

#include "book/book_xml_css_style_utils.h"
#include "book/epub_css_class_map.h"
#include "shared/string_utils.h"
#include "shared/text_token_constants.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace book_xml_css_resolver {

// ---------------------------------------------------------------------------
// Attribute extraction
// ---------------------------------------------------------------------------

// Return the value of the first "style" attribute found in attr[], or "".
std::string ExtractStyleAttr(const char **attr);

// Return the value of the first "class" attribute found in attr[], or "".
std::string ExtractClassAttr(const char **attr);

// ---------------------------------------------------------------------------
// CSS length
// ---------------------------------------------------------------------------

// Parse a CSS/HTML length value (px, %, em, rem, pt, cm, mm, in, vw, bare
// integer) and return pixels (>0), or 0 if unconstrained or unrecognised.
// text_width = available text area in pixels; font_px = current font height.
int ParseCssLengthPx(const char *v, int text_width, int font_px);

// Return the author-specified max width in pixels for an img element.
// Checks the HTML width attribute first, then the CSS width property in style.
// Returns 0 if no usable constraint found.
int ParseImgWidthPx(const char *width_attr, const char *style,
                    int text_width, int font_px);

// ---------------------------------------------------------------------------
// Inline style flags (bold, italic, underline, …)
// ---------------------------------------------------------------------------

// Resolve inline style flags from the element's style and class attributes.
// Each out-pointer is optional; only set, never cleared.
void ParseElementStyleFlags(const char **attr, bool *bold_out,
                            bool *italic_out, bool *underline_out,
                            uint8_t *underline_style_out, bool *overline_out,
                            bool *strikethrough_out, bool *superscript_out,
                            bool *subscript_out, bool *no_underline_out,
                            bool *reset_bold_out, bool *reset_italic_out);

// ---------------------------------------------------------------------------
// Visibility / hidden
// ---------------------------------------------------------------------------

// Set *hidden_out if the inline style value indicates the element is hidden.
void ParseInlineHiddenFlags(const char *style, bool *hidden_out);

// Set *hidden_out if the class value matches known screen-reader-only tokens.
void ParseClassHiddenFlags(const char *class_name, bool *hidden_out);

// Set *hidden_out from all attributes (hidden, aria-hidden, style, class).
void ParseElementHiddenFlags(const char **attr, bool *hidden_out);

// ---------------------------------------------------------------------------
// Text alignment
// ---------------------------------------------------------------------------

// True if the inline style contains "display: block" or "display:block".
bool StyleLooksDisplayBlock(const std::string &style_attr);

// True if the element tag (or its inline style) can carry a block text-align
// marker that child elements should inherit.
bool ElementCanCarryBlockTextAlign(const char *el,
                                   const std::string &style_attr);

// Resolve the effective text-align for an element.
// Priority: inline style > class > element-type rule > inherited stack > Left.
// element_tag (e.g. "p") enables lookup of "p { text-align: ... }" rules.
book_xml_css_style_utils::TextAlign ResolveElementTextAlignWithClass(
    const std::string &style_attr, const std::string &class_attr,
    const bool *block_text_align_stack, const uint8_t *block_text_align_value_stack,
    uint8_t stacksize, const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag = nullptr);

// ---------------------------------------------------------------------------
// Margin resolution
// ---------------------------------------------------------------------------

// Parse the margin-top from the element's style attribute.
book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopPx(const char **attr);

// Resolve margin-bottom from a previously captured style/class pair.
// Priority: inline style > class rule > element-type rule.
// element_tag (e.g. "p") enables lookup of "p { margin-bottom: ... }" rules.
book_xml_css_style_utils::MarginTopResult ParseElementMarginBottomWithClass(
    const std::string &last_style, const std::string &last_class,
    const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag = nullptr);

} // namespace book_xml_css_resolver
