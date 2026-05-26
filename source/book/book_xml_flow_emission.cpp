/*
    3dslibris - book_xml_flow_emission.cpp

    Flow emission extracted from book_xml_parser.cpp.

    Handles text fragment queuing, inline-tail coalescing, white-space
    normalisation, BiDi shaping, overflow detection, paragraph-start guard,
    and deferred style sync.
*/

#include "book/book_xml_flow_emission.h"

#include "book/book.h"
#include "book/book_xml_screen_advance.h"
#include "ui/text.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_text_emit.h"
#include "book/epub_css_class_map.h"
#include "parse.h"
#include "shared/debug_log.h"
#include "shared/text_layout_utils.h"
#include "shared/text_render_layout_utils.h"
#include "shared/text_unicode_utils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

static void SyncParsedTextStyleLocal(Text *ts, bool bold, bool italic,
                                     bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}

struct ParsedTextMeasureContext {
  parsedata_t *parsedata;
  Text *text;
  u8 style;
  u8 pixel_size;
  int latin1_cache_slot;
};

static void ResetLatin1AdvanceCacheSlot(parsedata_t *p, int slot, u8 style,
                                        u8 pixel_size) {
  if (!p)
    return;
  p->latin1_advance_cache_style[slot] = style;
  p->latin1_advance_cache_pixel_size[slot] = pixel_size;
  for (int i = 0; i < 8; i++)
    p->latin1_advance_cache_valid[slot][i] = 0;
}

static int FindLatin1AdvanceCacheSlot(parsedata_t *p, u8 style,
                                      u8 pixel_size) {
  if (!p)
    return -1;

  for (int slot = 0; slot < LATIN1_ADVANCE_CACHE_SLOTS; slot++) {
    if (p->latin1_advance_cache_style[slot] == style &&
        p->latin1_advance_cache_pixel_size[slot] == pixel_size)
      return slot;
  }

  const int slot =
      (int)(p->latin1_advance_cache_next_slot % LATIN1_ADVANCE_CACHE_SLOTS);
  p->latin1_advance_cache_next_slot =
      (u8)((p->latin1_advance_cache_next_slot + 1) %
           LATIN1_ADVANCE_CACHE_SLOTS);
  ResetLatin1AdvanceCacheSlot(p, slot, style, pixel_size);
  return slot;
}

static ParsedTextMeasureContext MakeParsedTextMeasureContext(parsedata_t *p,
                                                             Text *text,
                                                             bool bold,
                                                             bool italic,
                                                             bool mono) {
  ParsedTextMeasureContext ctx{};
  ctx.parsedata = p;
  ctx.text = text;
  ctx.style =
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono);
  ctx.pixel_size = text ? text->GetPixelSize() : 0;
  ctx.latin1_cache_slot =
      text ? FindLatin1AdvanceCacheSlot(p, ctx.style, ctx.pixel_size) : -1;
  return ctx;
}

static int MeasureParsedTextAdvance(uint32_t codepoint, void *ctx) {
  ParsedTextMeasureContext *measure = (ParsedTextMeasureContext *)ctx;
  if (!measure || !measure->text)
    return 0;
  if (codepoint < 256 && measure->parsedata &&
      measure->latin1_cache_slot >= 0) {
    parsedata_t *p = measure->parsedata;
    const int slot = measure->latin1_cache_slot;

    const int word = (int)(codepoint >> 5);
    const u32 mask = (u32)1 << (codepoint & 31);
    if (p->latin1_advance_cache_valid[slot][word] & mask)
      return p->latin1_advance_cache[slot][codepoint];

    const int advance = measure->text->GetAdvance(codepoint, measure->style);
    p->latin1_advance_cache[slot][codepoint] = (u8)advance;
    p->latin1_advance_cache_valid[slot][word] |= mask;
    return advance;
  }
  return measure->text->GetAdvance(codepoint, measure->style);
}

#ifdef DSLIBRIS_DEBUG
struct FlushPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit FlushPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~FlushPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_flush_calls++;
    parsedata->perf_flush_ms += (u64)(osGetTime() - t_begin);
  }
};
#else
struct FlushPerfScope {
  explicit FlushPerfScope(parsedata_t *) {}
};
#endif

static book_xml_css_style_utils::WhiteSpaceMode
ResolveActiveWhiteSpaceLocal(const parsedata_t *p) {
  using book_xml_css_style_utils::WhiteSpaceMode;
  if (!p)
    return WhiteSpaceMode::Normal;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (p->style_white_space_stack[i] != 0)
      return (WhiteSpaceMode)(p->style_white_space_stack[i] - 1);
  }
  if (parse_in((parsedata_t *)p, TAG_PRE))
    return WhiteSpaceMode::PreWrap;
  return WhiteSpaceMode::Normal;
}

struct OverflowThunkCtx {
  const FlowEmissionFns *fns;
};

static void OverflowThunkLocal(parsedata_t *p, int lh, void *ctx) {
  const OverflowThunkCtx *c = (const OverflowThunkCtx *)ctx;
  c->fns->advance_page_overflow(p, lh);
}

static void EmitFlowedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx,
    const book_xml_text_emit::FlowEmitMetrics &emit_metrics,
    const FlowEmissionFns &fns) {
  if (!p || !txt || txtlen == 0)
    return;

  std::vector<text_layout_utils::ShapedGlyph> &run = p->shaped_run;
  bool has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &run, &has_rtl, &p->bidi_cps, &p->bidi_runs))
    return;

  OverflowThunkCtx thunk_ctx{&fns};
  book_xml_text_emit::EmitFlowedShapedText(
      p, txt, run, has_rtl, p->bidi_runs, emit_metrics,
      OverflowThunkLocal, &thunk_ctx);
}

static bool FlowedTextFitsCurrentVisualLine(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx,
    const book_xml_text_emit::FlowEmitMetrics &emit_metrics,
    book_xml_css_style_utils::WhiteSpaceMode white_space) {
  if (!p || !txt || txtlen == 0)
    return false;
  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap)
    return false;

  std::string normalized;
  const char *measure_txt = txt;
  size_t measure_len = txtlen;
  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Normal ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::Nowrap ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::PreLine) {
    normalized = book_xml_css_style_utils::NormalizeWhiteSpaceText(
        txt, txtlen, white_space);
    measure_txt = normalized.c_str();
    measure_len = normalized.size();
  }
  if (measure_len == 0)
    return true;
  for (size_t i = 0; i < measure_len; i++) {
    if (measure_txt[i] == '\n' || measure_txt[i] == '\r')
      return false;
  }

  std::vector<text_layout_utils::ShapedGlyph> &run = p->shaped_run;
  bool has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          measure_txt, measure_len, NULL, MeasureParsedTextAdvance,
          (void *)&measure_ctx, &run, &has_rtl, &p->bidi_cps, &p->bidi_runs))
    return false;

  size_t start = 0;
  while (start < run.size() && run[start].text.whitespace &&
         run[start].text.breakable_space) {
    start++;
  }
  if (start >= run.size())
    return true;

  const int text_wrap_right_guard_px =
      emit_metrics.display_width > 32 ? 2 : 0;
  const int content_right =
      emit_metrics.display_width - emit_metrics.margin_right -
      text_wrap_right_guard_px;
  int start_x = p->linebegan ? p->pen.x : emit_metrics.margin_left;
  const int available = content_right - start_x;
  if (available <= 0)
    return false;

  return text_layout_utils::MeasureTextRun(run, start, run.size()) < available;
}

static bool IsSimpleParagraphTextFlush(const parsedata_t *p) {
  // Use in_paragraph rather than checking the stack top: inline elements
  // (<span>, <em>, etc.) push their own tags, so the top would not be TAG_P
  // even though we are still emitting the first text of the paragraph.
  // The caller already guards with !p->paragraph_has_content.
  return p && p->in_paragraph;
}

static void EmitPreformattedUtf8Segment(
    parsedata_t *p, const char *txt, size_t txtlen,
    const ParsedTextMeasureContext &measure_ctx, int lineheight,
    int linespacing, bool allow_wrap, bool text_already_transformed,
    const FlowEmissionFns &fns) {
  if (!p || !txt || txtlen == 0)
    return;

  Text *ts = p->ts;
  if (!allow_wrap) {
    size_t offset = 0;
    while (offset < txtlen) {
      uint32_t cp = 0;
      const size_t step = text_unicode_utils::DecodeNextDisplayCodepoint(
          txt + offset, txtlen - offset, &cp);
      if (step == 0) {
        offset++;
        continue;
      }
      if (cp == '\r') {
        offset += step;
        continue;
      }
      if (cp == '\n') {
        parse_append_page_byte(p, '\n');
        p->pen.x = ts->margin.left;
        p->pen.y += (lineheight + linespacing);
        p->linebegan = false;
        fns.advance_page_overflow(p, lineheight);
        offset += step;
        continue;
      }

      fns.advance_page_overflow(p, lineheight);
      if (text_already_transformed)
        book_xml_text_emit::AppendParsedCodepointsRaw(p, txt + offset, step);
      else
        book_xml_text_emit::AppendParsedCodepoints(p, txt + offset, step);
      p->pen.x += MeasureParsedTextAdvance(cp, (void *)&measure_ctx);
      p->linebegan = true;
      offset += step;
    }
    return;
  }

  std::vector<text_layout_utils::ShapedGlyph> &pre_run = p->shaped_run;
  bool pre_has_rtl = false;
  if (!text_layout_utils::ShapeTextRunBidi(
          txt, txtlen, NULL, MeasureParsedTextAdvance, (void *)&measure_ctx,
          &pre_run, &pre_has_rtl, &p->bidi_cps, &p->bidi_runs)) {
    return;
  }

  if (pre_has_rtl) {
    if (book_xml_text_emit::DetectParagraphRTL(pre_run))
      parse_append_page_byte(p, TEXT_PARAGRAPH_RTL);
    else
      parse_append_page_byte(p, TEXT_PARAGRAPH_LTR);
  }

  const int max_pre_line_width =
      ts->display.width - ts->margin.right - ts->margin.left;
  size_t unit_index = 0;
  while (unit_index < pre_run.size()) {
    const text_layout_utils::ShapedGlyph &unit = pre_run[unit_index];
    if (unit.text.codepoint == '\r') {
      unit_index++;
      continue;
    }
    if (unit.text.codepoint == '\n') {
      parse_append_page_byte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      fns.advance_page_overflow(p, lineheight);
      unit_index++;
      continue;
    }

    text_layout_utils::LineBreakMeasureResult segment =
        text_layout_utils::FindPreformattedLineBreakAndMeasure(
            pre_run, unit_index, max_pre_line_width);
    size_t segment_end_index = segment.end_index;
    if (segment_end_index <= unit_index)
      segment_end_index = unit_index + 1;

    size_t segment_start = pre_run[unit_index].text.byte_offset;
    size_t segment_end =
        pre_run[segment_end_index - 1].text.byte_offset +
        pre_run[segment_end_index - 1].text.byte_length;
    const int advance = segment.width;

    if (text_layout_utils::PreformattedSegmentNeedsNewLine(
            p->pen.x, advance, ts->display.width - ts->margin.right)) {
      parse_append_page_byte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      fns.advance_page_overflow(p, lineheight);
    }

    if (pre_has_rtl) {
      parse_append_page_byte(p, TEXT_RTL_LINE_PX);
      parse_append_page_byte(p, (u32)advance);
      book_xml_text_emit::EmitBidiSegment(p, pre_run, unit_index,
                                          segment_end_index, p->bidi_runs,
                                          !text_already_transformed);
    } else {
      if (text_already_transformed) {
        book_xml_text_emit::AppendParsedCodepointsRaw(
            p, txt + segment_start, segment_end - segment_start);
      } else {
        book_xml_text_emit::AppendParsedCodepoints(
            p, txt + segment_start, segment_end - segment_start);
      }
    }
    p->pen.x += advance;
    p->linebegan = true;
    unit_index = segment_end_index;

    if (unit_index < pre_run.size() &&
        pre_run[unit_index].text.codepoint != '\n') {
      parse_append_page_byte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      fns.advance_page_overflow(p, lineheight);
    }
  }
}

static void FlushInlineTextTail(parsedata_t *p, const FlowEmissionFns &fns) {
  if (!p || p->inline_text_tail.empty())
    return;
  FlushPerfScope perf_scope(p);
  std::string tail;
  tail.swap(p->inline_text_tail);
  book_xml_flow_emission::EmitFlowedFragmentRaw(p, tail.c_str(), (int)tail.size(), fns);
}

} // namespace

namespace book_xml_flow_emission {

void ApplyDeferredStyleSync(parsedata_t *p, Text *ts) {
  if (!p || !p->deferred_style_sync)
    return;

  bool style_changed = false;
  if (p->bold != p->deferred_target_bold) {
    parse_append_page_byte(p, p->deferred_target_bold ? TEXT_BOLD_ON : TEXT_BOLD_OFF);
    if (p->deferred_target_bold)
      p->pos++;
    p->bold = p->deferred_target_bold;
    style_changed = true;
  }
  if (p->italic != p->deferred_target_italic) {
    parse_append_page_byte(p, p->deferred_target_italic ? TEXT_ITALIC_ON
                                                        : TEXT_ITALIC_OFF);
    p->italic = p->deferred_target_italic;
    style_changed = true;
  }
  if (p->underline != p->deferred_target_underline) {
    parse_append_page_byte(p, p->deferred_target_underline ? TEXT_UNDERLINE_ON
                                                           : TEXT_UNDERLINE_OFF);
    p->underline = p->deferred_target_underline;
    if (!p->underline)
      p->underline_style = UNDERLINE_STYLE_SOLID;
    style_changed = true;
  }
  if (p->underline &&
      p->underline_style != p->deferred_target_underline_style) {
    p->underline_style = p->deferred_target_underline_style;
    book_xml_parser_style_utils::EmitUnderlineStyleMarker(
        p, p->underline_style);
  }
  if (p->overline != p->deferred_target_overline) {
    parse_append_page_byte(p, p->deferred_target_overline ? TEXT_OVERLINE_ON
                                                          : TEXT_OVERLINE_OFF);
    p->overline = p->deferred_target_overline;
    style_changed = true;
  }
  if (p->strikethrough != p->deferred_target_strikethrough) {
    parse_append_page_byte(p, p->deferred_target_strikethrough
                                  ? TEXT_STRIKETHROUGH_ON
                                  : TEXT_STRIKETHROUGH_OFF);
    p->strikethrough = p->deferred_target_strikethrough;
    style_changed = true;
  }
  if (p->superscript != p->deferred_target_superscript) {
    parse_append_page_byte(p, p->deferred_target_superscript
                                  ? TEXT_SUPERSCRIPT_ON
                                  : TEXT_SUPERSCRIPT_OFF);
    p->superscript = p->deferred_target_superscript;
    style_changed = true;
  }
  if (p->subscript != p->deferred_target_subscript) {
    parse_append_page_byte(p, p->deferred_target_subscript ? TEXT_SUBSCRIPT_ON
                                                           : TEXT_SUBSCRIPT_OFF);
    p->subscript = p->deferred_target_subscript;
    style_changed = true;
  }
  if (p->mono != p->deferred_target_mono) {
    parse_append_page_byte(p, p->deferred_target_mono ? TEXT_MONO_ON
                                                      : TEXT_MONO_OFF);
    p->mono = p->deferred_target_mono;
    style_changed = true;
  }

  p->deferred_style_sync = false;
  if (style_changed)
    SyncParsedTextStyleLocal(ts, p->bold, p->italic, p->mono);
}

void EmitFlowedFragmentRaw(parsedata_t *p, const char *txt, int txtlen,
                           const FlowEmissionFns &fns) {
  if (!p || !txt || txtlen <= 0)
    return;

  Text *ts = p->ts;
  SyncParsedTextStyleLocal(ts, p->bold, p->italic, p->mono);
  const ParsedTextMeasureContext measure_ctx =
      MakeParsedTextMeasureContext(p, ts, p->bold, p->italic, p->mono);

  int lineheight = ts->GetHeight();
  int linespacing = ts->linespacing;
  int spaceadvance = MeasureParsedTextAdvance((u16)' ', (void *)&measure_ctx);

  std::string transformed_text;
  const char *flow_txt = txt;
  size_t flow_txtlen = (size_t)txtlen;
  bool text_already_transformed = false;
  if (parse_resolve_text_transform(p) != 0) {
    transformed_text =
        book_xml_text_emit::TransformUtf8ForLayout(p, txt, (size_t)txtlen);
    flow_txt = transformed_text.c_str();
    flow_txtlen = transformed_text.size();
    text_already_transformed = true;
  }

  const book_xml_css_style_utils::WhiteSpaceMode white_space =
      ResolveActiveWhiteSpaceLocal(p);
  book_xml_text_emit::FlowEmitMetrics emit_metrics{};
  emit_metrics.display_width = ts->display.width;
  emit_metrics.base_margin_left = ts->margin.left;
  emit_metrics.margin_left = ts->margin.left + p->block_margin_left;
  emit_metrics.margin_right = ts->margin.right + p->block_margin_right;
  emit_metrics.lineheight = lineheight;
  emit_metrics.linespacing = linespacing;
  emit_metrics.spaceadvance = spaceadvance;
  emit_metrics.text_already_transformed = text_already_transformed;

  if (p->in_paragraph && !p->paragraph_has_content &&
      p->book->GetPublisherTextIndentEnabled()) {
    using book_xml_css_style_utils::MarginTopResult;
    MarginTopResult ti = book_xml_css_style_utils::ParseTextIndent(
        p->last_p_style.c_str());
    if (ti.unit == MarginTopResult::Unit::None)
      ti = epub_css_class_map::LookupTextIndentForClassAttr(
          p->last_p_class, p->css_class_map);
    // CSS text-indent is inherited — fall back to the parent div's class if
    // neither the inline style nor the <p> class defines one.
    if (ti.unit == MarginTopResult::Unit::None && !p->last_div_class.empty())
      ti = epub_css_class_map::LookupTextIndentForClassAttr(
          p->last_div_class, p->css_class_map);
// TEXTINDENT_TRACE: per-paragraph TextIndent diagnostics. Off by default;
// fires once per paragraph (thousands per large EPUB), enough to slow parse
// noticeably via fflush. Flip to 1 only when debugging text-indent rules.
#ifndef TEXTINDENT_TRACE
#define TEXTINDENT_TRACE 0
#endif
#if defined(DSLIBRIS_DEBUG) && TEXTINDENT_TRACE
    {
      const char *unit_str = (ti.unit == MarginTopResult::Unit::None) ? "none" :
                             (ti.unit == MarginTopResult::Unit::Px) ? "px" :
                             (ti.unit == MarginTopResult::Unit::Percent) ? "%" :
                             (ti.unit == MarginTopResult::Unit::Em) ? "em" : "?";
      DBG_LOGF(p->book->GetStatusReporter(),
        "TextIndent cls=%s sty=%s unit=%s val=%d neg=%d dispw=%d pxsz=%d",
        p->last_p_class.empty() ? "-" : p->last_p_class.c_str(),
        p->last_p_style.empty() ? "-" : p->last_p_style.c_str(),
        unit_str, ti.value, ti.negative ? 1 : 0,
        ts->display.width, (int)ts->GetPixelSize());
    }
#endif
    if (ti.unit != MarginTopResult::Unit::None && !ti.negative) {
      const int px = book_xml_css_style_utils::ResolveHorizontalMarginPx(
          ti, ts->display.width, (int)ts->GetPixelSize());
#if defined(DSLIBRIS_DEBUG) && TEXTINDENT_TRACE
      DBG_LOGF(p->book->GetStatusReporter(),
        "TextIndent resolved px=%d (applied=%d)", px, px > 0 ? 1 : 0);
#endif
      if (px > 0)
        emit_metrics.text_indent_px = px;
    }
  }

  if (p->buflen == 0) {
    p->pen.x = ts->margin.left;
    p->pen.y = ts->margin.top + lineheight;
    p->linebegan = false;
    book_xml_screen_advance::ClearPendingBlockSpacing(p); // already at top of screen; discard spacing
  } else {
    // Flush any accumulated layout-only block spacing before the first real
    // text token using the two-phase content-aware rule.
    fns.flush_pending_block(p, "text");

    // Paragraph-start quality guard: if this is the first text content of a
    // normal flowed paragraph and the current screen has fewer than 2 visible
    // line slots remaining, advance to the next screen/page before emitting.
    if (p->in_paragraph && !p->paragraph_has_content &&
        !book_xml_screen_advance::IsCurrentReadingScreenVisuallyEmpty(p)) {
      const book_xml_css_style_utils::WhiteSpaceMode ws_mode =
          ResolveActiveWhiteSpaceLocal(p);
      const bool is_pre =
          ws_mode == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
          ws_mode == book_xml_css_style_utils::WhiteSpaceMode::PreWrap;
      if (!is_pre) {
        const int lh = ts->GetHeight();
        const int ls = ts->linespacing;
        const int step = lh + (ls > 0 ? ls : 0);
        if (step > 0) {
          const text_render_layout_utils::ReadingScreenMetrics sm =
              text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
                  p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
                  text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
          const bool slot1 =
              text_render_layout_utils::CurrentLineFitsScreen(
                  p->pen.y, lh, ls, sm.max_height, sm.bottom_margin);
          const bool slot2 =
              text_render_layout_utils::HasRoomForFollowingLine(
                  p->pen.y, lh, ls, sm.max_height, sm.bottom_margin);
          bool one_line_paragraph = false;
          if (slot1 && !slot2 && IsSimpleParagraphTextFlush(p)) {
            one_line_paragraph = FlowedTextFitsCurrentVisualLine(
                p, flow_txt, flow_txtlen, measure_ctx, emit_metrics,
                white_space);
          }
          const bool should_advance =
              text_render_layout_utils::ShouldAdvanceParagraphStartGuard(
                  slot1, slot2, one_line_paragraph);
          if (should_advance) {
            fns.advance_screen(p);
          }
        }
      }
    }
  }
  // Mark screen as having drawable content only when this text node will
  // actually produce visible glyphs. Whitespace-only text between block
  // elements (e.g. "\n    " between <body> and <h1>) must not count —
  // it produces no glyphs and should not suppress top-of-page margin
  // collapsing for the first heading.
  // Exception 1: pre/pre-wrap modes — whitespace is significant content.
  // Exception 2: already on a line (lb=true) — inline spaces are meaningful.
  if (!p->current_screen_has_drawable_content) {
    const bool is_pre_mode =
        white_space == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
        white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap;
    bool has_drawable = is_pre_mode || p->linebegan;
    if (!has_drawable) {
      for (size_t i = 0; i < flow_txtlen && !has_drawable; i++) {
        const unsigned char c = (unsigned char)flow_txt[i];
        // Any byte > 0x7F is part of a multi-byte UTF-8 sequence (non-ASCII
        // character). ASCII non-whitespace also counts as drawable.
        if (c > 0x7F || (c != ' ' && c != '\t' && c != '\n' && c != '\r'))
          has_drawable = true;
      }
    }
    if (has_drawable)
      p->current_screen_has_drawable_content = true;
  }

  {
    const text_render_layout_utils::ReadingScreenMetrics sm =
        text_render_layout_utils::ResolveReadingScreenMetricsForReadingScreen(
            p->book->GetOrientation() != 0, p->screen, ts->margin.bottom,
            text_render_layout_utils::ResolveCompactReadingBottomMargin(ts->margin.bottom));
    emit_metrics.overflow_threshold = sm.max_height - sm.bottom_margin;
    emit_metrics.screen_max_height = sm.max_height;
    emit_metrics.screen_bottom_margin = sm.bottom_margin;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Pre ||
      white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap) {
    EmitPreformattedUtf8Segment(
        p, flow_txt, flow_txtlen, measure_ctx, lineheight, linespacing,
        white_space == book_xml_css_style_utils::WhiteSpaceMode::PreWrap,
        text_already_transformed, fns);
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::Nowrap) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            flow_txt, flow_txtlen, white_space);
    if (!normalized.empty())
      EmitFlowedUtf8Segment(p, normalized.c_str(), normalized.size(),
                            measure_ctx, emit_metrics, fns);
    return;
  }

  if (white_space == book_xml_css_style_utils::WhiteSpaceMode::PreLine) {
    const std::string normalized =
        book_xml_css_style_utils::NormalizeWhiteSpaceText(
            flow_txt, flow_txtlen, white_space);
    size_t start = 0;
    while (start <= normalized.size()) {
      const size_t nl = normalized.find('\n', start);
      const size_t end =
          (nl == std::string::npos) ? normalized.size() : nl;
      if (end > start) {
        EmitFlowedUtf8Segment(p, normalized.c_str() + start, end - start,
                              measure_ctx, emit_metrics, fns);
      }
      if (nl == std::string::npos)
        break;
      parse_append_page_byte(p, '\n');
      p->pen.x = ts->margin.left;
      p->pen.y += (lineheight + linespacing);
      p->linebegan = false;
      fns.advance_page_overflow(p, lineheight);
      start = nl + 1;
    }
    return;
  }

  EmitFlowedUtf8Segment(p, flow_txt, flow_txtlen, measure_ctx, emit_metrics,
                        fns);
}

void QueueFlowedFragmentRaw(parsedata_t *p, const char *txt, int txtlen,
                            const FlowEmissionFns &fns) {
  if (!p || !txt || txtlen <= 0)
    return;
  if (!p->coalesce_text_segments) {
    EmitFlowedFragmentRaw(p, txt, txtlen, fns);
    return;
  }

  static const size_t kInlineTextTailFlushBytes = 4096;
  const size_t len = (size_t)txtlen;
  if (!p->inline_text_tail.empty() &&
      p->inline_text_tail.size() + len > kInlineTextTailFlushBytes)
    FlushInlineTextTail(p, fns);
  if (len > kInlineTextTailFlushBytes) {
    EmitFlowedFragmentRaw(p, txt, txtlen, fns);
    return;
  }
  p->inline_text_tail.append(txt, len);
}

void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts,
                                     const FlowEmissionFns &fns) {
  FlushInlineTextTail(p, fns);
  ApplyDeferredStyleSync(p, ts);
}

} // namespace book_xml_flow_emission
