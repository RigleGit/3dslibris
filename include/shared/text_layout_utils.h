#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/text_bidi_utils.h"
#include "shared/text_unicode_utils.h"

namespace text_layout_utils {

typedef int (*MeasureCodepointFn)(uint32_t codepoint, void *ctx);

struct ShapedGlyph {
  text_unicode_utils::TextCodepoint text;
  int advance;
  uint8_t bidi_level; // 0=LTR (default), odd=RTL
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
                      std::vector<ShapedGlyph> *out,
                      bool *contains_rtl = nullptr,
                      bool *contains_arabic = nullptr);
bool ShapeTextRunBidi(const char *s, size_t len, const char *lang,
                       MeasureCodepointFn measure_codepoint, void *measure_ctx,
                       std::vector<ShapedGlyph> *out, bool *has_rtl,
                       std::vector<uint32_t> *bidi_cps = nullptr,
                       std::vector<text_bidi_utils::BidiRun> *bidi_runs = nullptr);
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
bool PreformattedSegmentNeedsNewLine(int pen_x, int advance, int right_edge);

// Apply Arabic contextual shaping and BIDI reordering to a UTF-8 string,
// producing a display-ready UTF-8 string.  Returns true if any RTL content
// was found.  *out_is_rtl is set to true when the paragraph base direction
// is RTL (first strong character heuristic).  Safe to call with a NULL
// measure function (uses a no-op so advances are not needed).
bool PrepareDisplayUtf8(const char *s, size_t len,
                        std::string *out_utf8, bool *out_is_rtl);

} // namespace text_layout_utils
