#pragma once

#include "parse.h"
#include "shared/text_bidi_utils.h"
#include "shared/text_layout_utils.h"

#include <string>

namespace book_xml_text_emit {

struct FlowEmitMetrics {
  int display_width;
  int base_margin_left;
  int margin_left;
  int margin_right;
  int lineheight;
  int linespacing;
  int spaceadvance;
  bool text_already_transformed;
};

typedef void (*AdvancePageOnOverflowFn)(parsedata_t *p, int lineheight,
                                        void *ctx);

bool ParsedBufferEndsWithWhitespace(const parsedata_t *p);
std::string TransformUtf8ForLayout(parsedata_t *p, const char *utf8,
                                   size_t utf8_len);
void AppendParsedCodepoints(parsedata_t *p, const char *utf8, size_t utf8_len);
void AppendParsedCodepointsRaw(parsedata_t *p, const char *utf8,
                               size_t utf8_len);
void EmitBidiSegment(parsedata_t *p,
                     const std::vector<text_layout_utils::ShapedGlyph> &run,
                     size_t seg_start, size_t seg_end,
                     const std::vector<text_bidi_utils::BidiRun> &runs,
                     bool apply_text_transform = true);
bool DetectParagraphRTL(
    const std::vector<text_layout_utils::ShapedGlyph> &run);

void EmitFlowedShapedText(
    parsedata_t *p, const char *txt,
    const std::vector<text_layout_utils::ShapedGlyph> &run, bool has_rtl,
    const std::vector<text_bidi_utils::BidiRun> &bidi_runs,
    const FlowEmitMetrics &metrics,
    AdvancePageOnOverflowFn advance_page_on_overflow, void *advance_ctx);

} // namespace book_xml_text_emit
