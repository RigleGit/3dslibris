#pragma once
// Internal support functions for book_xml_parser.cpp, extracted from
// anonymous namespace into a named namespace so they can be shared across
// translation units while remaining invisible to external callers.

#include "parse.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_flow_emission.h"

class Text;

namespace book_xml_parser_support {

void AppendParsedByte(parsedata_t *p, u32 c);
bool HasVisibleTextContentUtf8(const char *txt, int txtlen);
bool ParseInAnyEasyParagraphTightBlock(const parsedata_t *p);
void QueueDeferredStyleSync(parsedata_t *p, bool want_bold, bool want_italic,
                            bool want_underline, u8 want_underline_style,
                            bool want_overline, bool want_strikethrough,
                            bool want_superscript, bool want_subscript,
                            bool want_mono);
bool ClassListContains(const char *class_attr, const char *needle);
bool AttrNameEquals(const char *name, const char *needle);
book_xml_css_style_utils::MarginTopResult
    ParseElementMarginTopPx(const char **attr);
int ParseImgWidthPx(const char *width_attr, const char *style,
                    int text_width, int font_px);
std::string ExtractStyleAttr(const char **attr);
std::string ExtractClassAttr(const char **attr);
void AlignFreshLineToBlockMargin(parsedata_t *p, Text *ts);
book_xml_css_style_utils::WhiteSpaceMode
    ResolveActiveWhiteSpace(const parsedata_t *p);
book_xml_css_style_utils::MarginTopResult
    ParseElementMarginBottomWithClass(const std::string &last_style,
                                      const std::string &last_class,
                                      const epub_css_class_map::CssClassMap &class_map,
                                      const char *element_tag = nullptr);
book_xml_css_style_utils::TextAlign ResolveElementTextAlignWithClass(
    const std::string &style_attr, const std::string &class_attr,
    const parsedata_t *p, const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag = nullptr);
void AppendParagraphAlignMarker(parsedata_t *p,
                                book_xml_css_style_utils::TextAlign align);
bool ElementCanCarryBlockTextAlign(const char *el,
                                   const std::string &style_attr);
void RestoreActiveBlockTextAlignMarker(parsedata_t *p);
bool ImagePathLooksLikeSvgWrapper(const std::string &path);
void LogResolvedBlockMargin(parsedata_t *p, const char *tag, const char *phase,
                            const std::string &style_attr,
                            const std::string &class_attr,
                            const book_xml_css_style_utils::MarginTopResult &m,
                            int line_h, int default_lf, int final_lf);
void ParseElementStyleFlags(const char **attr, bool *bold_out,
                            bool *italic_out, bool *underline_out,
                            u8 *underline_style_out, bool *overline_out,
                            bool *strikethrough_out, bool *superscript_out,
                            bool *subscript_out, bool *no_underline_out,
                            bool *reset_bold_out, bool *reset_italic_out);
void ParseElementHiddenFlags(const char **attr, bool *hidden_out);
bool HasActiveStackBoldStyle(const parsedata_t *p);
bool HasActiveStackHiddenStyle(const parsedata_t *p);
bool HasActiveStackItalicStyle(const parsedata_t *p);
bool HasActiveStackUnderlineStyle(const parsedata_t *p);
u8   ResolveActiveUnderlineStyle(const parsedata_t *p);
bool HasActiveStackOverlineStyle(const parsedata_t *p);
bool HasActiveStackStrikethroughStyle(const parsedata_t *p);
bool HasActiveStackSuperscriptStyle(const parsedata_t *p);
bool HasActiveStackSubscriptStyle(const parsedata_t *p);
bool HasActiveStackNoUnderlineStyle(const parsedata_t *p);
bool HasActiveStackResetBoldStyle(const parsedata_t *p);
bool HasActiveStackResetItalicStyle(const parsedata_t *p);
bool HasActiveStackMonoStyle(const parsedata_t *p);

#ifdef DSLIBRIS_DEBUG
struct ChardataPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;
  explicit ChardataPerfScope(parsedata_t *p) : parsedata(p), t_begin(osGetTime()) {}
  ~ChardataPerfScope() {
    if (!parsedata) return;
    parsedata->perf_chardata_calls++;
    parsedata->perf_chardata_ms += (u64)(osGetTime() - t_begin);
  }
};
struct ElementPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;
  explicit ElementPerfScope(parsedata_t *p) : parsedata(p), t_begin(osGetTime()) {}
  ~ElementPerfScope() {
    if (!parsedata) return;
    parsedata->perf_element_calls++;
    parsedata->perf_element_ms += (u64)(osGetTime() - t_begin);
  }
};
#else
struct ChardataPerfScope { explicit ChardataPerfScope(parsedata_t *) {} };
struct ElementPerfScope  { explicit ElementPerfScope(parsedata_t *)  {} };
#endif

void linefeed(parsedata_t *p);
void linefeed_r(parsedata_t *p, const char *, const char *, int);
bool blankline(parsedata_t *p);
void ApplyClearBreak(parsedata_t *p);
void RestoreParsedInlineLinkMarker(parsedata_t *p);
bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p);
void ClearPendingBlockSpacing(parsedata_t *p);
void AdvanceParsedPageOnOverflow(parsedata_t *p, int lh);
void AdvanceParsedScreen(parsedata_t *p);
void ForcePageBreak(parsedata_t *p);
void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                            const char *reason, bool from_css);
void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                        const char *reason);
void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf);
void FlushPendingBlockSpacingBeforeContent(parsedata_t *p, const char *tag);
FlowEmissionFns MakeFlowEmissionFns();
void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono);
void ApplyDeferredStyleSync(parsedata_t *p, Text *ts);
void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts);
void QueueFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt, int txtlen);

std::string NormalizeDocPath(const std::string &path);
bool XmlNameEquals(const char *name, const char *needle);
bool PathLooksLikeTocDoc(const std::string &path);
bool DocLooksLikeTocDoc(const parsedata_t *p);
std::string ResolveDocPath(const std::string &base_doc_path,
                           const std::string &href);
std::string NormalizeFb2ChapterTitle(const std::string &in);

bool IsBlockLevelElement(const char *el);
bool BehavesAsBlock(const char *el,
                    const epub_css_class_map::CssClassMargins &elem_css);
bool IsFigureContainerElement(const char *el, const char *class_attr);
void FlushInlineTailBeforeElementStart(parsedata_t *p, Text *ts,
                                       const char *el);
void FlushInlineTailBeforeElementEnd(parsedata_t *p, Text *ts, const char *el);
void SetCurrentStackHidden(parsedata_t *p, bool hidden);

} // namespace book_xml_parser_support
