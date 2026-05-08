#include "book/book_xml_text_emit.h"

#include "shared/text_token_constants.h"
#include "shared/text_render_layout_utils.h"
#include "utf8proc.h"

#include <algorithm>
#include <string>

namespace book_xml_text_emit {

namespace {

bool IsClosingAttachedPunctuation(uint32_t cp) {
  switch (cp) {
    case '.': case ',': case ';': case ':': case '!': case '?':
    case ')': case ']': case '}':
    case 0x2019: // RIGHT SINGLE QUOTATION MARK '
    case 0x201D: // RIGHT DOUBLE QUOTATION MARK "
    case 0x00BB: // RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK »
    case 0x203A: // SINGLE RIGHT-POINTING ANGLE QUOTATION MARK ›
    case 0x2026: // HORIZONTAL ELLIPSIS …
      return true;
    default:
      return false;
  }
}

bool SegmentIsOnlyClosingAttachedPunctuation(
    const std::vector<text_layout_utils::ShapedGlyph> &run, size_t start,
    size_t end) {
  if (start >= end || end > run.size())
    return false;
  for (size_t i = start; i < end; i++) {
    if (!IsClosingAttachedPunctuation(run[i].text.codepoint))
      return false;
  }
  return true;
}

void AdvancePageIfNeeded(parsedata_t *p, int lineheight,
                         AdvancePageOnOverflowFn advance_page_on_overflow,
                         void *advance_ctx) {
  if (advance_page_on_overflow)
    advance_page_on_overflow(p, lineheight, advance_ctx);
}

void AlignFreshLineToEffectiveLeftMargin(parsedata_t *p,
                                         const FlowEmitMetrics &metrics) {
  if (!p || p->linebegan)
    return;

  p->pen.x = metrics.margin_left;
}

void EmitFreshLineStartX(parsedata_t *p, const FlowEmitMetrics &metrics) {
  if (!p || p->linebegan || p->pen.x == metrics.base_margin_left)
    return;
  parse_append_page_byte(p, TEXT_LINE_START_X);
  parse_append_page_byte(p, (u32)p->pen.x);
}

bool CurrentLineFitsEmitMetrics(int pen_y, const FlowEmitMetrics &metrics) {
  if (metrics.screen_max_height > 0 && metrics.screen_bottom_margin >= 0) {
    return text_render_layout_utils::CurrentLineFitsScreen(
        pen_y, metrics.lineheight, metrics.linespacing,
        metrics.screen_max_height, metrics.screen_bottom_margin);
  }
  return (metrics.overflow_threshold <= 0) ||
         (pen_y <= metrics.overflow_threshold);
}

} // namespace

bool ParsedBufferEndsWithWhitespace(const parsedata_t *p) {
  if (!p || p->buflen == 0)
    return false;
  const u32 c = p->buf[p->buflen - 1];
  return c == ' ' || c == '\n' || c == '\t';
}

static uint32_t ApplyTextTransform(uint32_t cp, u8 transform,
                                   bool *word_start) {
  if (transform == 1) {
    return (uint32_t)utf8proc_toupper((utf8proc_int32_t)cp);
  } else if (transform == 2) {
    return (uint32_t)utf8proc_tolower((utf8proc_int32_t)cp);
  } else if (transform == 3 && word_start) {
    const bool was_start = *word_start;
    *word_start = (cp == ' ' || cp == '\t' || cp == '\n');
    if (was_start)
      return (uint32_t)utf8proc_toupper((utf8proc_int32_t)cp);
  }
  return cp;
}

static void AppendUtf8Codepoint(std::string *out, uint32_t cp) {
  if (!out)
    return;
  utf8proc_uint8_t encoded[4];
  const utf8proc_ssize_t len =
      utf8proc_encode_char((utf8proc_int32_t)cp, encoded);
  if (len <= 0)
    return;
  out->append((const char *)encoded, (size_t)len);
}

std::string TransformUtf8ForLayout(parsedata_t *p, const char *utf8,
                                   size_t utf8_len) {
  if (!utf8 || utf8_len == 0)
    return std::string();

  const u8 transform = parse_resolve_text_transform(p);
  if (!transform)
    return std::string(utf8, utf8_len);

  std::string out;
  out.reserve(utf8_len);
  size_t offset = 0;
  while (offset < utf8_len) {
    uint32_t cp = 0;
    size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        utf8 + offset, utf8_len - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    cp = ApplyTextTransform(cp, transform,
                            p ? &p->text_transform_word_start : NULL);
    AppendUtf8Codepoint(&out, cp);
    offset += consumed;
  }
  return out;
}

static void AppendParsedCodepointsImpl(parsedata_t *p, const char *utf8,
                                       size_t utf8_len,
                                       bool apply_text_transform) {
  const u8 transform =
      apply_text_transform ? parse_resolve_text_transform(p) : 0;
  size_t offset = 0;
  while (offset < utf8_len) {
    uint32_t cp = 0;
    size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        utf8 + offset, utf8_len - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    if (transform)
      cp = ApplyTextTransform(cp, transform, &p->text_transform_word_start);
    parse_append_page_byte(p, (u32)cp);
    offset += consumed;
  }
}

void AppendParsedCodepoints(parsedata_t *p, const char *utf8, size_t utf8_len) {
  AppendParsedCodepointsImpl(p, utf8, utf8_len, true);
}

void AppendParsedCodepointsRaw(parsedata_t *p, const char *utf8,
                               size_t utf8_len) {
  AppendParsedCodepointsImpl(p, utf8, utf8_len, false);
}

void EmitBidiSegment(parsedata_t *p,
                     const std::vector<text_layout_utils::ShapedGlyph> &run,
                     size_t seg_start, size_t seg_end,
                     const std::vector<text_bidi_utils::BidiRun> &runs,
                     bool apply_text_transform) {
  if (seg_start >= seg_end)
    return;
  std::vector<uint32_t> cps;
  cps.reserve(seg_end - seg_start);
  for (size_t i = seg_start; i < seg_end; i++)
    cps.push_back(run[i].text.codepoint);

  std::vector<text_bidi_utils::BidiRun> local_runs;
  local_runs.reserve(runs.size());
  for (size_t r = 0; r < runs.size(); r++) {
    const text_bidi_utils::BidiRun &br = runs[r];
    const size_t run_start = br.start;
    const size_t run_end = br.start + br.length;
    const size_t i_start = (run_start < seg_start) ? seg_start : run_start;
    const size_t i_end = (run_end > seg_end) ? seg_end : run_end;
    if (i_start >= i_end)
      continue;

    text_bidi_utils::BidiRun local;
    local.start = i_start - seg_start;
    local.length = i_end - i_start;
    local.bidi_level = br.bidi_level;
    local_runs.push_back(local);
  }

  text_bidi_utils::ReorderLineForDisplay(cps, 0, cps.size(), local_runs);
  const u8 transform =
      apply_text_transform ? parse_resolve_text_transform(p) : 0;
  for (size_t i = 0; i < cps.size(); i++) {
    uint32_t cp = cps[i];
    if (transform)
      cp = ApplyTextTransform(cp, transform, &p->text_transform_word_start);
    parse_append_page_byte(p, (u32)cp);
  }
}

bool DetectParagraphRTL(
    const std::vector<text_layout_utils::ShapedGlyph> &run) {
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

void EmitFlowedShapedText(
    parsedata_t *p, const char *txt,
    const std::vector<text_layout_utils::ShapedGlyph> &run, bool has_rtl,
    const std::vector<text_bidi_utils::BidiRun> &bidi_runs,
    const FlowEmitMetrics &metrics,
    AdvancePageOnOverflowFn advance_page_on_overflow, void *advance_ctx) {
  if (!p || !txt)
    return;

  if (has_rtl) {
    if (DetectParagraphRTL(run))
      parse_append_page_byte(p, TEXT_PARAGRAPH_RTL);
    else
      parse_append_page_byte(p, TEXT_PARAGRAPH_LTR);
  }

  const int maxLineWidth =
      metrics.display_width - metrics.margin_right - metrics.margin_left;
  size_t unit_index = 0;
  AlignFreshLineToEffectiveLeftMargin(p, metrics);

  while (unit_index < run.size()) {
    const text_layout_utils::ShapedGlyph &unit = run[unit_index];
    if (unit.text.codepoint == '\r') {
      unit_index++;
      continue;
    }
    if (has_rtl) {
      if (p->linebegan && p->pen.x > metrics.margin_left) {
        // Save pre-wrap pen.y: overflow check must use it (matches renderer).
        const int pen_y_before = p->pen.y;
        parse_append_page_byte(p, '\n');
        p->pen.x = metrics.margin_left;
        p->linebegan = false;
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
        // Advance pen.y only if no screen advance occurred.
        if (p->pen.y == pen_y_before)
          p->pen.y += (metrics.lineheight + metrics.linespacing);
      }

      if (p->in_paragraph)
        p->paragraph_has_content = true;

      while (unit_index < run.size() && run[unit_index].text.whitespace &&
             run[unit_index].text.breakable_space) {
        unit_index++;
      }
      if (unit_index >= run.size() || run[unit_index].text.codepoint == '\n')
        continue;

      const size_t line_start = unit_index;
      size_t line_end = unit_index;
      int line_px = 0;
      size_t scan = unit_index;

      while (scan < run.size()) {
        const uint32_t scp = run[scan].text.codepoint;
        if (scp == '\r') {
          scan++;
          continue;
        }
        if (scp == '\n')
          break;

        size_t space_start = scan;
        while (scan < run.size() && run[scan].text.whitespace &&
               run[scan].text.breakable_space) {
          scan++;
        }
        if (scan >= run.size() || run[scan].text.codepoint == '\n')
          break;

        text_layout_utils::LineBreakMeasureResult word =
            text_layout_utils::FindLineBreakAndMeasure(run, scan, maxLineWidth);
        size_t word_end = word.end_index > scan ? word.end_index : scan + 1;
        int word_px =
            (int)text_layout_utils::MeasureTextRun(run, scan, word_end);
        int space_px = (line_px > 0)
                           ? (int)text_layout_utils::MeasureTextRun(run,
                                                                    space_start,
                                                                    scan)
                           : 0;
        if (line_px > 0 && line_px + space_px + word_px > maxLineWidth)
          break;

        line_px += space_px + word_px;
        scan = word_end;
        line_end = scan;
      }

      while (line_end > line_start && run[line_end - 1].text.whitespace &&
             run[line_end - 1].text.breakable_space) {
        line_end--;
      }
      if (line_end <= line_start)
        line_end = line_start + 1;

      AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                          advance_ctx);
      parse_append_page_byte(p, TEXT_RTL_LINE_PX);
      parse_append_page_byte(p, (u32)line_px);
      EmitBidiSegment(p, run, line_start, line_end, bidi_runs,
                      !metrics.text_already_transformed);
      p->linebegan = true;
      p->pen.x = metrics.margin_left + (u16)line_px;

      unit_index = line_end;
      while (unit_index < run.size() && run[unit_index].text.whitespace &&
             run[unit_index].text.breakable_space) {
        unit_index++;
      }

      if (unit_index < run.size() && run[unit_index].text.codepoint != '\n') {
        const int pen_y_before = p->pen.y;
        parse_append_page_byte(p, '\n');
        p->pen.x = metrics.margin_left;
        p->linebegan = false;
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
        if (p->pen.y == pen_y_before)
          p->pen.y += (metrics.lineheight + metrics.linespacing);
      }
      continue;
    }

    if (unit.text.whitespace) {
      if (unit.text.breakable_space && p->linebegan &&
          !ParsedBufferEndsWithWhitespace(p)) {
        parse_append_page_byte(p, ' ');
        p->pen.x += metrics.spaceadvance;
      } else if (!unit.text.breakable_space) {
        u16 unit_advance = (u16)unit.advance;
        const int pen_y_before_nbsp = p->pen.y;
        bool nbsp_did_wrap = false;
        if ((p->pen.x + unit_advance) >=
            (metrics.display_width - metrics.margin_right)) {
          parse_append_page_byte(p, '\n');
          p->pen.x = metrics.margin_left;
          p->linebegan = false;
          nbsp_did_wrap = true;
        }
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
        if (nbsp_did_wrap && p->pen.y == pen_y_before_nbsp)
          p->pen.y += (metrics.lineheight + metrics.linespacing);
        EmitFreshLineStartX(p, metrics);
        if (metrics.text_already_transformed) {
          AppendParsedCodepointsRaw(p, txt + unit.text.byte_offset,
                                    unit.text.byte_length);
        } else {
          AppendParsedCodepoints(p, txt + unit.text.byte_offset,
                                 unit.text.byte_length);
        }
        p->pen.x += unit_advance;
        p->linebegan = true;
      }
      unit_index++;
      continue;
    }

    if (p->in_paragraph)
      p->paragraph_has_content = true;

    text_layout_utils::LineBreakMeasureResult segment =
        text_layout_utils::FindLineBreakAndMeasure(run, unit_index,
                                                   maxLineWidth);
    size_t segment_end_index = segment.end_index;
    if (segment_end_index <= unit_index)
      segment_end_index = unit_index + 1;
    u16 advance = (u16)segment.width;
    size_t segment_start = unit.text.byte_offset;
    size_t segment_end = run[segment_end_index - 1].text.byte_offset +
                         run[segment_end_index - 1].text.byte_length;

    const bool attached_closing_punctuation =
        SegmentIsOnlyClosingAttachedPunctuation(run, unit_index,
                                                segment_end_index);
    // Punctuation glue: if a trailing space was already emitted into the
    // buffer just before this closing-punctuation segment (e.g. from a
    // collapsed newline at an inline style boundary), remove it so the
    // punctuation appears adjacent to the previous word.
    if (attached_closing_punctuation && p->linebegan &&
        p->buflen > 0 && p->buf[p->buflen - 1] == (u32)' ') {
      p->buflen--;
      p->pen.x = (p->pen.x > (int)metrics.spaceadvance)
                     ? (p->pen.x - metrics.spaceadvance)
                     : metrics.margin_left;
    }
    const bool need_wrap =
        ((p->pen.x + advance) >= (metrics.display_width - metrics.margin_right) &&
         !(p->linebegan && attached_closing_punctuation));
    if (need_wrap) {
      parse_append_page_byte(p, '\n');
      p->pen.x = metrics.margin_left;
      p->pen.y += (metrics.lineheight + metrics.linespacing);
      p->linebegan = false;
    }
    // Advance to the next screen/page only if the candidate line (p->pen.y
    // after any wrap) itself cannot be drawn. A candidate line may be valid
    // even when there is no room for a following line.
    {
      if (!CurrentLineFitsEmitMetrics(p->pen.y, metrics)) {
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
      }
    }
    EmitFreshLineStartX(p, metrics);
    if (has_rtl)
      EmitBidiSegment(p, run, unit_index, segment_end_index, bidi_runs,
                      !metrics.text_already_transformed);
    else if (metrics.text_already_transformed)
      AppendParsedCodepointsRaw(p, txt + segment_start,
                                segment_end - segment_start);
    else
      AppendParsedCodepoints(p, txt + segment_start, segment_end - segment_start);
    p->linebegan = true;
    p->pen.x += advance;
    unit_index = segment_end_index;
  }
}

} // namespace book_xml_text_emit
