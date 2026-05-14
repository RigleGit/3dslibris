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

} // namespace page_alignment_utils
