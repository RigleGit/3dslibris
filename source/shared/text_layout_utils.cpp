#include "shared/text_layout_utils.h"

namespace text_layout_utils {

bool ShapeTextRunUtf8(const char *s, size_t len, const char *lang,
                      MeasureCodepointFn measure_codepoint, void *measure_ctx,
                      std::vector<ShapedGlyph> *out) {
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
    out->push_back(glyph);
  }
  return true;
}

int MeasureTextRun(const std::vector<ShapedGlyph> &run, size_t start,
                   size_t end) {
  int width = 0;
  if (end > run.size())
    end = run.size();
  for (size_t i = start; i < end; i++)
    width += run[i].advance;
  return width;
}

size_t FindLineBreak(const std::vector<ShapedGlyph> &run, size_t start,
                     int max_width) {
  if (start >= run.size())
    return run.size();

  int width = 0;
  size_t last_break = start;
  bool have_break = false;
  size_t i = start;
  for (; i < run.size(); i++) {
    const ShapedGlyph &glyph = run[i];
    if (glyph.text.codepoint == '\r')
      continue;
    if (glyph.text.whitespace)
      return i;

    width += glyph.advance;
    if (glyph.text.must_break_after) {
      last_break = i + 1;
      have_break = true;
    } else if (glyph.text.allow_break_after &&
               i + 1 < run.size() && !run[i + 1].text.whitespace) {
      last_break = i + 1;
      have_break = true;
    }

    if (width > max_width) {
      if (have_break && last_break > start)
        return last_break;
      return (i > start) ? i : (i + 1);
    }
  }

  return i;
}

} // namespace text_layout_utils
