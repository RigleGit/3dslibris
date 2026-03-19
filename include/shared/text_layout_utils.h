#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "shared/text_unicode_utils.h"

namespace text_layout_utils {

typedef int (*MeasureCodepointFn)(uint32_t codepoint, void *ctx);

struct ShapedGlyph {
  text_unicode_utils::TextCodepoint text;
  int advance;
};

bool ShapeTextRunUtf8(const char *s, size_t len, const char *lang,
                      MeasureCodepointFn measure_codepoint, void *measure_ctx,
                      std::vector<ShapedGlyph> *out);
int MeasureTextRun(const std::vector<ShapedGlyph> &run, size_t start,
                   size_t end);
size_t FindLineBreak(const std::vector<ShapedGlyph> &run, size_t start,
                     int max_width);

} // namespace text_layout_utils
