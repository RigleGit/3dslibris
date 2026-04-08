#include "book/book_xml_text_emit.h"

#include "debug_log.h"
#include "shared/text_token_constants.h"

#include <algorithm>

namespace book_xml_text_emit {

namespace {

void AdvancePageIfNeeded(parsedata_t *p, int lineheight,
                         AdvancePageOnOverflowFn advance_page_on_overflow,
                         void *advance_ctx) {
  if (advance_page_on_overflow)
    advance_page_on_overflow(p, lineheight, advance_ctx);
}

} // namespace

bool ParsedBufferEndsWithWhitespace(const parsedata_t *p) {
  if (!p || p->buflen == 0)
    return false;
  const u32 c = p->buf[p->buflen - 1];
  return c == ' ' || c == '\n' || c == '\t';
}

void AppendParsedCodepoints(parsedata_t *p, const char *utf8, size_t utf8_len) {
  size_t offset = 0;
  while (offset < utf8_len) {
    uint32_t cp = 0;
    size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        utf8 + offset, utf8_len - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    parse_append_page_byte(p, (u32)cp);
    offset += consumed;
  }
}

void EmitBidiSegment(parsedata_t *p,
                     const std::vector<text_layout_utils::ShapedGlyph> &run,
                     size_t seg_start, size_t seg_end,
                     const std::vector<text_bidi_utils::BidiRun> &runs) {
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
  for (size_t i = 0; i < cps.size(); i++)
    parse_append_page_byte(p, (u32)cps[i]);
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

  while (unit_index < run.size()) {
    const text_layout_utils::ShapedGlyph &unit = run[unit_index];
    if (unit.text.codepoint == '\r') {
      unit_index++;
      continue;
    }
    if (has_rtl) {
      if (p->linebegan && p->pen.x > metrics.margin_left) {
        parse_append_page_byte(p, '\n');
        p->pen.x = metrics.margin_left;
        p->pen.y += (metrics.lineheight + metrics.linespacing);
        p->linebegan = false;
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
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
      EmitBidiSegment(p, run, line_start, line_end, bidi_runs);
      p->linebegan = true;
      p->pen.x = metrics.margin_left + (u16)line_px;

      unit_index = line_end;
      while (unit_index < run.size() && run[unit_index].text.whitespace &&
             run[unit_index].text.breakable_space) {
        unit_index++;
      }

      if (unit_index < run.size() && run[unit_index].text.codepoint != '\n') {
        parse_append_page_byte(p, '\n');
        p->pen.x = metrics.margin_left;
        p->pen.y += (metrics.lineheight + metrics.linespacing);
        p->linebegan = false;
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
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
        if ((p->pen.x + unit_advance) >
            (metrics.display_width - metrics.margin_right)) {
          parse_append_page_byte(p, '\n');
          p->pen.x = metrics.margin_left;
          p->pen.y += (metrics.lineheight + metrics.linespacing);
          p->linebegan = false;
        }
        AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                            advance_ctx);
        AppendParsedCodepoints(p, txt + unit.text.byte_offset,
                               unit.text.byte_length);
        p->pen.x += unit_advance;
        p->linebegan = true;
      }
      unit_index++;
      continue;
    }

    if (p->in_paragraph)
      p->paragraph_has_content = true;

    size_t segment_start = unit.text.byte_offset;
    text_layout_utils::LineBreakMeasureResult segment =
        text_layout_utils::FindLineBreakAndMeasure(run, unit_index,
                                                   maxLineWidth);
    size_t segment_end_index = segment.end_index;
    if (segment_end_index <= unit_index)
      segment_end_index = unit_index + 1;
    u16 advance = (u16)segment.width;
    size_t segment_end = run[segment_end_index - 1].text.byte_offset +
                         run[segment_end_index - 1].text.byte_length;

    if ((p->pen.x + advance) > (metrics.display_width - metrics.margin_right)) {
      parse_append_page_byte(p, '\n');
      p->pen.x = metrics.margin_left;
      p->pen.y += (metrics.lineheight + metrics.linespacing);
      p->linebegan = false;
    }

    AdvancePageIfNeeded(p, metrics.lineheight, advance_page_on_overflow,
                        advance_ctx);
    if (has_rtl)
      EmitBidiSegment(p, run, unit_index, segment_end_index, bidi_runs);
    else
      AppendParsedCodepoints(p, txt + segment_start, segment_end - segment_start);
    p->linebegan = true;
    p->pen.x += advance;
    unit_index = segment_end_index;
  }
}

} // namespace book_xml_text_emit
