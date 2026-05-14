// Support functions for book_xml_parser.cpp — adapter wrappers, inline-state
// helpers, text-flow utilities, and path normalisation. Previously lived in
// book_xml_parser.cpp's anonymous namespace.

#include "book/book_xml_parser_support.h"

#include "book/book_xml_block_utils.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_hidden_utils.h"
#include "book/epub_css_class_map.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_screen_advance.h"
#include "book/book_xml_text_emit.h"
#include "formats/common/epub_image_utils.h"
#include "shared/string_utils.h"
#include "shared/text_unicode_utils.h"
#include "ui/text.h"
#include "parse.h"

#include <algorithm>
#include <string>
#include <vector>

namespace book_xml_parser_support {

void AppendParsedByte(parsedata_t *p, u32 c) {
  parse_append_page_byte(p, c);
}

bool HasVisibleTextContentUtf8(const char *txt, int txtlen) {
  if (!txt || txtlen <= 0)
    return false;
  size_t offset = 0;
  while (offset < (size_t)txtlen) {
    uint32_t cp = 0;
    const size_t consumed = text_unicode_utils::DecodeNextDisplayCodepoint(
        txt + offset, (size_t)txtlen - offset, &cp);
    if (consumed == 0) {
      offset++;
      continue;
    }
    if (!iswhitespace((u32)cp))
      return true;
    offset += consumed;
  }
  return false;
}

bool ParseInAnyEasyParagraphTightBlock(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (book_xml_block_utils::SuppressInnerParagraphSpacing(p->stack[i]))
      return true;
  }
  return false;
}

void QueueDeferredStyleSync(parsedata_t *p, bool want_bold, bool want_italic,
                            bool want_underline, u8 want_underline_style,
                            bool want_overline, bool want_strikethrough,
                            bool want_superscript, bool want_subscript,
                            bool want_mono) {
  book_xml_inline_state::QueueDeferredStyleSync(
      p, want_bold, want_italic, want_underline, want_underline_style,
      want_overline, want_strikethrough, want_superscript, want_subscript,
      want_mono);
}

bool ClassListContains(const char *class_attr, const char *needle) {
  if (!class_attr || !needle || !needle[0])
    return false;

  const std::string classes = ToLowerAscii(std::string(class_attr));
  const std::string target = ToLowerAscii(std::string(needle));
  size_t pos = 0;
  while (pos < classes.size()) {
    while (pos < classes.size() &&
           (classes[pos] == ' ' || classes[pos] == '\t' ||
            classes[pos] == '\r' || classes[pos] == '\n'))
      pos++;
    const size_t start = pos;
    while (pos < classes.size() &&
           classes[pos] != ' ' && classes[pos] != '\t' &&
           classes[pos] != '\r' && classes[pos] != '\n')
      pos++;
    if (pos > start && classes.substr(start, pos - start) == target)
      return true;
  }
  return false;
}

bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopPx(const char **attr) {
  return book_xml_css_resolver::ParseElementMarginTopPx(attr);
}

int ParseImgWidthPx(const char *width_attr, const char *style,
                    int text_width, int font_px) {
  return book_xml_css_resolver::ParseImgWidthPx(width_attr, style, text_width,
                                                 font_px);
}

std::string ExtractStyleAttr(const char **attr) {
  return book_xml_css_resolver::ExtractStyleAttr(attr);
}

std::string ExtractClassAttr(const char **attr) {
  return book_xml_css_resolver::ExtractClassAttr(attr);
}

void AlignFreshLineToBlockMargin(parsedata_t *p, Text *ts) {
  if (!p || !ts)
    return;
  const int x = std::max(0, ts->margin.left + p->block_margin_left);
  if (p->pen.x == x)
    return;
  p->pen.x = x;
  if (!p->linebegan) {
    AppendParsedByte(p, TEXT_LINE_START_X);
    AppendParsedByte(p, (u32)x);
  }
}

book_xml_css_style_utils::WhiteSpaceMode
ResolveActiveWhiteSpace(const parsedata_t *p) {
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

book_xml_css_style_utils::MarginTopResult
ParseElementMarginBottomWithClass(const std::string &last_style,
                                  const std::string &last_class,
                                  const epub_css_class_map::CssClassMap &class_map,
                                  const char *element_tag) {
  return book_xml_css_resolver::ParseElementMarginBottomWithClass(
      last_style, last_class, class_map, element_tag);
}

book_xml_css_style_utils::TextAlign ResolveElementTextAlignWithClass(
    const std::string &style_attr, const std::string &class_attr,
    const parsedata_t *p, const epub_css_class_map::CssClassMap &class_map,
    const char *element_tag) {
  if (!p)
    return book_xml_css_style_utils::TextAlign::Left;
  return book_xml_css_resolver::ResolveElementTextAlignWithClass(
      style_attr, class_attr, p->block_text_align_stack,
      p->block_text_align_value_stack, p->stacksize, class_map, element_tag);
}

void AppendParagraphAlignMarker(parsedata_t *p,
                                book_xml_css_style_utils::TextAlign align) {
  if (!p)
    return;
  if (align == book_xml_css_style_utils::TextAlign::Center) {
    AppendParsedByte(p, TEXT_PARAGRAPH_CENTER);
  } else if (align == book_xml_css_style_utils::TextAlign::Right) {
    AppendParsedByte(p, TEXT_PARAGRAPH_RIGHT);
  } else {
    AppendParsedByte(p, TEXT_PARAGRAPH_LEFT);
  }
}

bool ElementCanCarryBlockTextAlign(const char *el,
                                   const std::string &style_attr) {
  return book_xml_css_resolver::ElementCanCarryBlockTextAlign(el, style_attr);
}

void RestoreActiveBlockTextAlignMarker(parsedata_t *p) {
  if (!p)
    return;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->block_text_align_stack[i])
      continue;
    AppendParagraphAlignMarker(
        p, (book_xml_css_style_utils::TextAlign)
               p->block_text_align_value_stack[i]);
    return;
  }
  AppendParagraphAlignMarker(p, book_xml_css_style_utils::TextAlign::Left);
}

bool ImagePathLooksLikeSvgWrapper(const std::string &path) {
  static const std::vector<u8> empty;
  return epub_image_utils::LooksLikeSvgWrapper(path, empty);
}

void LogResolvedBlockMargin(parsedata_t *p, const char *tag, const char *phase,
                            const std::string &style_attr,
                            const std::string &class_attr,
                            const book_xml_css_style_utils::MarginTopResult &m,
                            int line_h, int default_lf, int final_lf) {
  (void)p; (void)tag; (void)phase; (void)style_attr;
  (void)class_attr; (void)m; (void)line_h; (void)default_lf; (void)final_lf;
}

void ParseElementStyleFlags(const char **attr, bool *bold_out,
                            bool *italic_out, bool *underline_out,
                            u8 *underline_style_out, bool *overline_out,
                            bool *strikethrough_out, bool *superscript_out,
                            bool *subscript_out, bool *no_underline_out,
                            bool *reset_bold_out, bool *reset_italic_out) {
  return book_xml_css_resolver::ParseElementStyleFlags(
      attr, bold_out, italic_out, underline_out,
      reinterpret_cast<uint8_t *>(underline_style_out),
      overline_out, strikethrough_out, superscript_out, subscript_out,
      no_underline_out, reset_bold_out, reset_italic_out);
}

void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  return book_xml_css_resolver::ParseElementHiddenFlags(attr, hidden_out);
}

bool HasActiveStackBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackBoldStyle(p);
}
bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackHiddenStyle(p);
}
bool HasActiveStackItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackItalicStyle(p);
}
bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackUnderlineStyle(p);
}
u8 ResolveActiveUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::ResolveActiveUnderlineStyle(p);
}
bool HasActiveStackOverlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackOverlineStyle(p);
}
bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackStrikethroughStyle(p);
}
bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSuperscriptStyle(p);
}
bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSubscriptStyle(p);
}
bool HasActiveStackNoUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackNoUnderlineStyle(p);
}
bool HasActiveStackResetBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetBoldStyle(p);
}
bool HasActiveStackResetItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetItalicStyle(p);
}
bool HasActiveStackMonoStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackMonoStyle(p);
}

void linefeed(parsedata_t *p) { book_xml_screen_advance::Linefeed(p); }
void linefeed_r(parsedata_t *p, const char *, const char *, int) {
  book_xml_screen_advance::Linefeed(p);
}
bool blankline(parsedata_t *p) { return book_xml_screen_advance::Blankline(p); }
void ApplyClearBreak(parsedata_t *p) { book_xml_screen_advance::ApplyClearBreak(p); }
void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
}
bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p) {
  return book_xml_screen_advance::IsCurrentReadingScreenVisuallyEmpty(p);
}
void ClearPendingBlockSpacing(parsedata_t *p) {
  book_xml_screen_advance::ClearPendingBlockSpacing(p);
}
void AdvanceParsedPageOnOverflow(parsedata_t *p, int lh) {
  book_xml_screen_advance::AdvanceParsedPageOnOverflow(p, lh);
}
void AdvanceParsedScreen(parsedata_t *p) {
  book_xml_screen_advance::AdvanceParsedScreen(p);
}
void ForcePageBreak(parsedata_t *p) {
  book_xml_screen_advance::ForcePageBreak(p);
}
void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                            const char *reason, bool from_css) {
  book_xml_screen_advance::QueueBlockSpacingLines(p, lines, tag, reason, from_css);
}
void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                        const char *reason) {
  book_xml_screen_advance::SuppressPendingBlockSpacingFromCss(p, tag, reason);
}
void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf) {
  book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
      p, tag, reason, mtr, line_h, default_lf);
}
void FlushPendingBlockSpacingBeforeContent(parsedata_t *p, const char *tag) {
  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, tag);
}

static void FlowAdvanceScreen(parsedata_t *p) {
  book_xml_screen_advance::AdvanceParsedScreen(p);
}
static void FlowAdvancePageOverflow(parsedata_t *p, int lh) {
  book_xml_screen_advance::AdvanceParsedPageOnOverflow(p, lh);
}
static void FlowFlushPendingBlock(parsedata_t *p, const char *tag) {
  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, tag);
}

FlowEmissionFns MakeFlowEmissionFns() {
  FlowEmissionFns f;
  f.advance_screen = FlowAdvanceScreen;
  f.advance_page_overflow = FlowAdvancePageOverflow;
  f.flush_pending_block = FlowFlushPendingBlock;
  return f;
}

void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}
void ApplyDeferredStyleSync(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::ApplyDeferredStyleSync(p, ts);
}
void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, MakeFlowEmissionFns());
}
void QueueFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt, int txtlen) {
  book_xml_flow_emission::QueueFlowedFragmentRaw(p, txt, txtlen, MakeFlowEmissionFns());
}

std::string NormalizeDocPath(const std::string &path) {
  std::string in = path;
  std::replace(in.begin(), in.end(), '\\', '/');
  while (!in.empty() && in[0] == '/')
    in.erase(in.begin());

  std::vector<std::string> parts;
  std::string cur;
  for (size_t i = 0; i <= in.size(); i++) {
    if (i == in.size() || in[i] == '/') {
      if (cur == "..") {
        if (!parts.empty())
          parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(in[i]);
    }
  }

  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i)
      out.push_back('/');
    out += parts[i];
  }
  return out;
}

bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

bool PathLooksLikeTocDoc(const std::string &path) {
  if (path.empty())
    return false;
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return lower.find("toc") != std::string::npos ||
         lower.find("indice") != std::string::npos ||
         lower.find("index") != std::string::npos ||
         lower.find("contents") != std::string::npos ||
         lower.find("contenido") != std::string::npos ||
         lower.find("nav") != std::string::npos;
}

bool DocLooksLikeTocDoc(const parsedata_t *p) {
  if (!p)
    return false;
  return PathLooksLikeTocDoc(p->docpath) || PathLooksLikeTocDoc(p->doc_title) ||
         PathLooksLikeTocDoc(p->doc_heading);
}

std::string ResolveDocPath(const std::string &base_doc_path,
                           const std::string &href) {
  if (href.empty())
    return "";
  if (href.find("://") != std::string::npos)
    return "";
  if (href.compare(0, 5, "data:") == 0)
    return "";

  std::string clean_href = href;
  size_t hash = clean_href.find('#');
  if (hash != std::string::npos)
    clean_href = clean_href.substr(0, hash);
  if (clean_href.empty())
    return "";

  if (!clean_href.empty() && clean_href[0] == '/')
    return NormalizeDocPath(clean_href);

  std::string base = base_doc_path;
  size_t slash = base.find_last_of('/');
  if (slash != std::string::npos)
    base = base.substr(0, slash + 1);
  else
    base.clear();

  return NormalizeDocPath(base + clean_href);
}

std::string NormalizeFb2ChapterTitle(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  bool pending_space = false;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty())
      out.push_back(' ');
    pending_space = false;
    out.push_back((char)c);
  }
  return out;
}

bool IsBlockLevelElement(const char *el) {
  return !strcmp(el, "p") || !strcmp(el, "div") ||
         !strcmp(el, "h1") || !strcmp(el, "h2") || !strcmp(el, "h3") ||
         !strcmp(el, "h4") || !strcmp(el, "h5") || !strcmp(el, "h6") ||
         !strcmp(el, "section") || !strcmp(el, "article") ||
         !strcmp(el, "aside") || !strcmp(el, "blockquote") ||
         !strcmp(el, "header") || !strcmp(el, "footer") ||
         !strcmp(el, "figure") || !strcmp(el, "dl") ||
         !strcmp(el, "dt") || !strcmp(el, "dd");
}

bool BehavesAsBlock(const char *el,
                    const epub_css_class_map::CssClassMargins &elem_css) {
  return IsBlockLevelElement(el) || elem_css.is_display_block;
}

bool IsFigureContainerElement(const char *el, const char *class_attr) {
  if (!el)
    return false;
  if (!strcmp(el, "figure"))
    return true;
  return !strcmp(el, "div") && ClassListContains(class_attr, "figure");
}

void FlushInlineTailBeforeElementStart(parsedata_t *p, Text *ts,
                                       const char *el) {
  if (!p)
    return;

  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

void FlushInlineTailBeforeElementEnd(parsedata_t *p, Text *ts, const char *el) {
  if (!p)
    return;

  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

void SetCurrentStackHidden(parsedata_t *p, bool hidden) {
  if (!p || p->stacksize == 0)
    return;
  p->style_hidden_stack[p->stacksize - 1] = hidden;
}

} // namespace book_xml_parser_support
