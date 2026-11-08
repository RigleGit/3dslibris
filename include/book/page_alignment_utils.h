#pragma once

#include <stddef.h>
#include <stdint.h>

namespace page_alignment_utils {

typedef int (*MeasureGlyphFn)(uint32_t codepoint, unsigned char style,
                              void *ctx);

int MeasureAlignedLineWidth(const uint32_t *buf, size_t length, size_t start,
                            bool bold, bool italic, bool mono,
                            MeasureGlyphFn measure, void *ctx);

int MeasureFirstVisualLineWidth(const uint32_t *buf, size_t length, size_t start,
                                bool bold, bool italic, bool mono,
                                int available_width,
                                MeasureGlyphFn measure, void *ctx);

// Result of scanning one wrapped line for justification.
struct JustifyLineScan {
  int width;         // Advance of the line without trailing spaces.
  int gaps;          // Spaces between words that may be widened.
  bool justifiable;  // Closed by a soft wrap and made of plain text only.
};

// Scans the line that starts at `start`. A line is justifiable only when it
// ends in TEXT_SOFT_WRAP + '\n' and holds no image, rule, preformatted text,
// font change or RTL content; the last line of a paragraph and lines before a
// <br> end in a hard '\n' and are never stretched.
JustifyLineScan ScanJustifyLine(const uint32_t *buf, size_t length,
                                size_t start, bool bold, bool italic,
                                bool mono, MeasureGlyphFn measure, void *ctx);

// Splits `available - line_width` over `gaps`: each gap grows by `base`, and
// the first `remainder` gaps by one more pixel. Returns false (no change) when
// there is nothing to distribute, the line already overflows, or the stretch
// per gap would exceed `max_extra_per_gap`.
bool ComputeJustifySpacing(int line_width, int available, int gaps,
                           int max_extra_per_gap, int *base, int *remainder);

int ComputeAlignedLineStartX(int base_margin_left, int base_margin_right,
                             int current_line_start_x, int display_width,
                             int line_width, int paragraph_align);

uint8_t ResolveBandImageAlignMode(uint8_t explicit_align_mode,
                                  int paragraph_align);

} // namespace page_alignment_utils
