/*
    3dslibris - book_xml_screen_advance.h

    Screen advancement and block spacing queue. Extracted from book_xml_parser.cpp.

    Covers: linefeed, page/screen overflow, pending block spacing model,
    and the FlushPendingBlockSpacingBeforeContent algorithm.
*/

#pragma once

struct parsedata_t;

namespace book_xml_css_style_utils {
struct MarginTopResult;
}

namespace book_xml_screen_advance {

// Emit a '\n' token and advance pen to the next line.
void Linefeed(parsedata_t *p);

// True if the last two bytes in the page buffer are both '\n' (blank line).
bool Blankline(parsedata_t *p);

// Emit a linefeed only if the current line has content and is not already blank.
void ApplyClearBreak(parsedata_t *p);

// True only when no drawable content has been placed on the current screen/page
// since the last advance. Checking pen.y is not sufficient; use this instead.
bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p);

// Zero out all pending block-spacing state.
void ClearPendingBlockSpacing(parsedata_t *p);

// If the current pen position would push past the bottom margin after
// adding lineheight, commit the current screen to a Page (if screen==1)
// or emit a TEXT_SCREEN_BREAK marker (if screen==0) and reset pen.
void AdvanceParsedPageOnOverflow(parsedata_t *p, int lineheight);

// Commit the current screen/page and reset pen to the top of the next screen.
void AdvanceParsedScreen(parsedata_t *p);

// Force a page break unconditionally (used for CSS page-break-before/after).
void ForcePageBreak(parsedata_t *p);

// Force a full logical page break. Ensures the next content starts at the top
// of screen 0 (new Page), not at the top of screen 1.
void ForceHardPageBreak(parsedata_t *p);

// Queue layout-only block spacing (mandatory break + optional extra lines).
// lines >= 1. from_css=true when sourced from an explicit CSS margin property.
void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                            const char *reason, bool from_css);

// Suppress optional spacing only (mandatory break is preserved).
void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                        const char *reason);

// Queue spacing derived from a parsed CSS margin value.
void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf);

// Flush all pending block spacing just before real content is emitted.
// Uses the two-phase content-aware algorithm (see implementation for details).
void FlushPendingBlockSpacingBeforeContent(parsedata_t *p,
                                           const char *next_tag);

} // namespace book_xml_screen_advance
