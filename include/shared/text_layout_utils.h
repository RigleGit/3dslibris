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

struct LineBreakMeasureResult {
  size_t end_index;
  int width;

  LineBreakMeasureResult() : end_index(0), width(0) {}
  LineBreakMeasureResult(size_t end_index_, int width_)
      : end_index(end_index_), width(width_) {}
};

struct PerfStats {
  uint64_t shape_ms;
  uint64_t measure_ms;
  uint64_t line_break_ms;
  uint64_t pre_line_break_ms;
  uint32_t shape_calls;
  uint32_t measure_calls;
  uint32_t line_break_calls;
  uint32_t pre_line_break_calls;
  uint32_t shaped_glyphs;

  PerfStats()
      : shape_ms(0), measure_ms(0), line_break_ms(0), pre_line_break_ms(0),
        shape_calls(0), measure_calls(0), line_break_calls(0),
        pre_line_break_calls(0), shaped_glyphs(0) {}
};

bool ShapeTextRunUtf8(const char *s, size_t len, const char *lang,
                      MeasureCodepointFn measure_codepoint, void *measure_ctx,
                      std::vector<ShapedGlyph> *out);
PerfStats GetPerfStats();
void ResetPerfStats();
int MeasureTextRun(const std::vector<ShapedGlyph> &run, size_t start,
                   size_t end);
size_t FindLineBreak(const std::vector<ShapedGlyph> &run, size_t start,
                     int max_width);
size_t FindPreformattedLineBreak(const std::vector<ShapedGlyph> &run,
                                 size_t start, int max_width);
LineBreakMeasureResult
FindLineBreakAndMeasure(const std::vector<ShapedGlyph> &run, size_t start,
                        int max_width);
LineBreakMeasureResult FindPreformattedLineBreakAndMeasure(
    const std::vector<ShapedGlyph> &run, size_t start, int max_width);

} // namespace text_layout_utils
