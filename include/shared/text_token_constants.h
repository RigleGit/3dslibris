#pragma once

// Text token constants — shared between format parsers and UI rendering.
// These magic numbers mark style transitions inside the text token stream.

#define TEXT_BOLD_ON 2
#define TEXT_BOLD_OFF 3
#define TEXT_ITALIC_ON 4
#define TEXT_ITALIC_OFF 5
#define TEXT_IMAGE 6
#define TEXT_IMAGE_LEADING_PARAGRAPH 7
#define TEXT_IMAGE_FIGURE_WITH_CAPTION 8
#define TEXT_IMAGE_CONTEXT_DEFAULT 14
#define TEXT_PARAGRAPH_RTL 15
#define TEXT_PARAGRAPH_LTR 16
#define TEXT_UNDERLINE_ON 18
#define TEXT_UNDERLINE_OFF 19
#define TEXT_STRIKETHROUGH_ON 20
#define TEXT_STRIKETHROUGH_OFF 21
#define TEXT_SUPERSCRIPT_ON 22
#define TEXT_SUPERSCRIPT_OFF 23
#define TEXT_SUBSCRIPT_ON 24
#define TEXT_SUBSCRIPT_OFF 25

#define TEXT_STYLE_REGULAR (u8)0
#define TEXT_STYLE_BOLD (u8)1
#define TEXT_STYLE_ITALIC (u8)2
#define TEXT_STYLE_BOLDITALIC (u8)3
#define TEXT_STYLE_BROWSER (u8)4
