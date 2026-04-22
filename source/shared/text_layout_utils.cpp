#include "shared/text_layout_utils.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <string>
#include <time.h>

#include "shared/text_arabic_shaping.h"
#include "shared/text_bidi_utils.h"

namespace text_layout_utils {

namespace {

PerfStats g_perf_stats;

static bool IsOpeningWordAttachedPunctuation(uint32_t cp) {
  return cp == 0x00A1 || cp == 0x00BF; // ¡ ¿
}

static bool IsClosingWordAttachedPunctuation(uint32_t cp) {
  return cp == '!' || cp == '?';
}

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
                 i + 1 < run.size() && !run[i + 1].text.whitespace &&
                 !IsOpeningWordAttachedPunctuation(glyph.text.codepoint) &&
                 !IsClosingWordAttachedPunctuation(run[i + 1].text.codepoint)) {
        last_break = i + 1;
        width_at_last_break = width;
        have_break = true;
      }
    }

    if (width > max_width) {
      if (have_break && last_break > start)
        return LineBreakMeasureResult(last_break, width_at_last_break);
      if (!preformatted && IsClosingWordAttachedPunctuation(glyph.text.codepoint) &&
          i > start + 1) {
        return LineBreakMeasureResult(i - 1,
                                      width - glyph.advance - run[i - 1].advance);
      }
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
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  if (!out || !measure_codepoint)
    return false;

  out->clear();
  static std::vector<text_unicode_utils::TextCodepoint> text_run;
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
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.shape_calls++;
  g_perf_stats.shaped_glyphs += (uint32_t)text_run.size();
  g_perf_stats.shape_ms += PerfNowMs() - t_begin;
#endif
  return true;
}

PerfStats GetPerfStats() { return g_perf_stats; }

void ResetPerfStats() { g_perf_stats = PerfStats(); }

int MeasureTextRun(const std::vector<ShapedGlyph> &run, size_t start,
                   size_t end) {
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  int width = 0;
  if (end > run.size())
    end = run.size();
  for (size_t i = start; i < end; i++)
    width += run[i].advance;
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.measure_calls++;
  g_perf_stats.measure_ms += PerfNowMs() - t_begin;
#endif
  return width;
}

size_t FindLineBreak(const std::vector<ShapedGlyph> &run, size_t start,
                     int max_width) {
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, false);
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.line_break_calls++;
  g_perf_stats.line_break_ms += PerfNowMs() - t_begin;
#endif
  return result.end_index;
}

size_t FindPreformattedLineBreak(const std::vector<ShapedGlyph> &run,
                                 size_t start, int max_width) {
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, true);
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.pre_line_break_calls++;
  g_perf_stats.pre_line_break_ms += PerfNowMs() - t_begin;
#endif
  return result.end_index;
}

LineBreakMeasureResult
FindLineBreakAndMeasure(const std::vector<ShapedGlyph> &run, size_t start,
                        int max_width) {
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, false);
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.line_break_calls++;
  g_perf_stats.line_break_ms += PerfNowMs() - t_begin;
#endif
  return result;
}

LineBreakMeasureResult FindPreformattedLineBreakAndMeasure(
    const std::vector<ShapedGlyph> &run, size_t start, int max_width) {
#ifdef DSLIBRIS_DEBUG
  const uint64_t t_begin = PerfNowMs();
#endif
  const LineBreakMeasureResult result =
      FindLineBreakImpl(run, start, max_width, true);
#ifdef DSLIBRIS_DEBUG
  g_perf_stats.pre_line_break_calls++;
  g_perf_stats.pre_line_break_ms += PerfNowMs() - t_begin;
#endif
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

namespace {

// No-op measurement used when only shaping/BIDI reorder is needed.
static int NoopMeasure(uint32_t /*cp*/, void * /*ctx*/) { return 1; }

// Encode a single Unicode codepoint to UTF-8; returns byte count (1-4).
static int EncodeUtf8Cp(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
}

// First-strong-character paragraph direction heuristic (UAX#9 P2/P3).
static bool DetectRtlParagraph(const std::vector<ShapedGlyph> &run) {
  for (size_t i = 0; i < run.size(); i++) {
    uint32_t cp = run[i].text.codepoint;
    if ((cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A) ||
        (cp >= 0x00C0 && cp <= 0x024F))
      return false;
    if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0x0600 && cp <= 0x06FF) ||
        (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFF))
      return true;
  }
  return false;
}

} // namespace

bool PrepareDisplayUtf8(const char *s, size_t len,
                        std::string *out_utf8, bool *out_is_rtl) {
  if (out_is_rtl) *out_is_rtl = false;
  if (!s || len == 0) {
    if (out_utf8) out_utf8->clear();
    return false;
  }

  std::vector<ShapedGlyph> run;
  bool has_rtl = false;
  if (!ShapeTextRunBidi(s, len, NULL, NoopMeasure, NULL, &run, &has_rtl)) {
    if (out_utf8) *out_utf8 = std::string(s, len);
    return false;
  }

  const bool is_rtl = has_rtl && DetectRtlParagraph(run);
  if (out_is_rtl) *out_is_rtl = is_rtl;

  if (!has_rtl) {
    // No RTL content: return original string (already correctly ordered).
    if (out_utf8) *out_utf8 = std::string(s, len);
    return false;
  }

  // Collect codepoints (already have Arabic presentation forms from shaping).
  std::vector<uint32_t> cps;
  cps.reserve(run.size());
  for (size_t i = 0; i < run.size(); i++)
    cps.push_back(run[i].text.codepoint);

  // Reconstruct bidi runs from per-glyph levels stamped by ShapeTextRunBidi.
  std::vector<text_bidi_utils::BidiRun> bidi_runs;
  if (!run.empty()) {
    text_bidi_utils::BidiRun cur;
    cur.start = 0;
    cur.length = 1;
    cur.bidi_level = (int)run[0].bidi_level;
    for (size_t i = 1; i < run.size(); i++) {
      if ((int)run[i].bidi_level == cur.bidi_level) {
        cur.length++;
      } else {
        bidi_runs.push_back(cur);
        cur.start = i;
        cur.length = 1;
        cur.bidi_level = (int)run[i].bidi_level;
      }
    }
    bidi_runs.push_back(cur);
  }

  // Apply UAX#9 L2 visual reordering to the full string.
  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), bidi_runs);

  // Encode reordered codepoints back to UTF-8.
  if (out_utf8) {
    out_utf8->clear();
    out_utf8->reserve(run.size() * 3);  // Arabic is typically 3 bytes/cp
    char buf[4];
    for (size_t i = 0; i < cps.size(); i++) {
      int bytes = EncodeUtf8Cp(cps[i], buf);
      out_utf8->append(buf, (size_t)bytes);
    }
  }
  return true;
}

} // namespace text_layout_utils
