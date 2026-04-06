#include "shared/text_layout_utils.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <time.h>

#include "shared/text_arabic_shaping.h"
#include "shared/text_bidi_utils.h"

namespace text_layout_utils {

namespace {

PerfStats g_perf_stats;

static uint64_t PerfNowMs() {
#ifdef __3DS__
  return (uint64_t)osGetTime();
#else
  return (uint64_t)(((double)clock() * 1000.0) / CLOCKS_PER_SEC);
#endif
}

static LineBreakMeasureResult FindLineBreakImpl(
    const std::vector<ShapedGlyph> &run, size_t start, int max_width,
    bool preformatted) {
  if (start >= run.size())
    return LineBreakMeasureResult(run.size(), 0);

  int width = 0;
  size_t last_break = start;
  int width_at_last_break = 0;
  bool have_break = false;
  size_t i = start;
  for (; i < run.size(); i++) {
    const ShapedGlyph &glyph = run[i];
    if (glyph.text.codepoint == '\r')
      continue;
    if (preformatted) {
      if (glyph.text.codepoint == '\n')
        return LineBreakMeasureResult(i, width);
    } else if (glyph.text.whitespace) {
      return LineBreakMeasureResult(i, width);
    }

    width += glyph.advance;
    if (!preformatted) {
      if (glyph.text.must_break_after) {
        last_break = i + 1;
        width_at_last_break = width;
        have_break = true;
      } else if (glyph.text.allow_break_after &&
                 i + 1 < run.size() && !run[i + 1].text.whitespace) {
        last_break = i + 1;
        width_at_last_break = width;
        have_break = true;
      }
    }

    if (width > max_width) {
      if (!preformatted && have_break && last_break > start)
        return LineBreakMeasureResult(last_break, width_at_last_break);
      if (i > start)
        return LineBreakMeasureResult(i, width - glyph.advance);
      return LineBreakMeasureResult(i + 1, width);
    }
  }

  return LineBreakMeasureResult(i, width);
}

} // namespace

bool ShapeTextRunUtf8(const char *s, size_t len, const char *lang,
                      MeasureCodepointFn measure_codepoint, void *measure_ctx,
                      std::vector<ShapedGlyph> *out) {
  const uint64_t t_begin = PerfNowMs();
  if (!out || !measure_codepoint)
    return false;

  out->clear();
  std::vector<text_unicode_utils::TextCodepoint> text_run;
  if (!text_unicode_utils::BuildTextRunUtf8(s, len, lang, &text_run))
    return false;

  out->reserve(text_run.size());
  for (size_t i = 0; i < text_run.size(); i++) {
    ShapedGlyph glyph;
    glyph.text = text_run[i];
    glyph.advance =
        measure_codepoint(text_run[i].codepoint, measure_ctx);
    glyph.bidi_level = 0;
    out->push_back(glyph);
  }
  g_perf_stats.shape_calls++;
  g_perf_stats.shaped_glyphs += (uint32_t)text_run.size();
  g_perf_stats.shape_ms += PerfNowMs() - t_begin;
  return true;
}

PerfStats GetPerfStats() { return g_perf_stats; }

void ResetPerfStats() { g_perf_stats = PerfStats(); }

int MeasureTextRun(const std::vector<ShapedGlyph> &run, size_t start,
                   size_t end) {
  const uint64_t t_begin = PerfNowMs();
  int width = 0;
  if (end > run.size())
    end = run.size();
  for (size_t i = start; i < end; i++)
    width += run[i].advance;
  g_perf_stats.measure_calls++;
  g_perf_stats.measure_ms += PerfNowMs() - t_begin;
  return width;
}

size_t FindLineBreak(const std::vector<ShapedGlyph> &run, size_t start,
                     int max_width) {
  const uint64_t t_begin = PerfNowMs();
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, false);
  g_perf_stats.line_break_calls++;
  g_perf_stats.line_break_ms += PerfNowMs() - t_begin;
  return result.end_index;
}

size_t FindPreformattedLineBreak(const std::vector<ShapedGlyph> &run,
                                 size_t start, int max_width) {
  const uint64_t t_begin = PerfNowMs();
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, true);
  g_perf_stats.pre_line_break_calls++;
  g_perf_stats.pre_line_break_ms += PerfNowMs() - t_begin;
  return result.end_index;
}

LineBreakMeasureResult
FindLineBreakAndMeasure(const std::vector<ShapedGlyph> &run, size_t start,
                        int max_width) {
  const uint64_t t_begin = PerfNowMs();
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, false);
  g_perf_stats.line_break_calls++;
  g_perf_stats.line_break_ms += PerfNowMs() - t_begin;
  return result;
}

LineBreakMeasureResult FindPreformattedLineBreakAndMeasure(
    const std::vector<ShapedGlyph> &run, size_t start, int max_width) {
  const uint64_t t_begin = PerfNowMs();
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, true);
  g_perf_stats.pre_line_break_calls++;
  g_perf_stats.pre_line_break_ms += PerfNowMs() - t_begin;
  return result;
}

bool ShapeTextRunBidi(const char *s, size_t len, const char *lang,
                      MeasureCodepointFn measure_codepoint, void *measure_ctx,
                      std::vector<ShapedGlyph> *out, bool *has_rtl) {
  if (has_rtl)
    *has_rtl = false;
  if (!ShapeTextRunUtf8(s, len, lang, measure_codepoint, measure_ctx, out))
    return false;
  if (!out || out->empty())
    return true;

  // Apply Arabic contextual shaping before BiDi so the analyser sees
  // presentation-form codepoints (still in RTL ranges, no BiDi change).
  text_arabic_shaping::ApplyContextualShaping(out, measure_codepoint,
                                               measure_ctx);

  std::vector<uint32_t> cps;
  cps.reserve(out->size());
  for (size_t i = 0; i < out->size(); i++)
    cps.push_back((*out)[i].text.codepoint);

  if (!text_bidi_utils::ContainsRTL(cps.data(), cps.size()))
    return true;

  if (has_rtl)
    *has_rtl = true;

  std::vector<text_bidi_utils::BidiRun> runs;
  text_bidi_utils::AnalyzeBidiRuns(cps.data(), cps.size(), &runs);

  for (size_t r = 0; r < runs.size(); r++) {
    const text_bidi_utils::BidiRun &run_info = runs[r];
    for (size_t j = 0; j < run_info.length; j++) {
      size_t idx = run_info.start + j;
      if (idx < out->size())
        (*out)[idx].bidi_level = (uint8_t)run_info.bidi_level;
    }
  }

  return true;
}

} // namespace text_layout_utils
