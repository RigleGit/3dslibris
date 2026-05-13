/*
    3dslibris - book.cpp
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Core book state/container logic shared across EPUB/FB2/TXT/RTF/ODT.
    - Chapter/bookmark management and TOC target resolution helpers.
    - Page ownership/lifetime and parser integration points.
*/

#include "book/book.h"

#include "book/book_xml_block_utils.h"
#include "book/book_context.h"
#include "book/book_xml_css_resolver.h"
#include "book/book_xml_inline_state.h"
#include "book/book_xml_css_style_utils.h"
#include "book/book_xml_hidden_utils.h"
#include "book/book_xml_list_utils.h"
#include "book/epub_css_class_map.h"
#include "book/book_xml_parser_style_utils.h"
#include "book/book_xml_table_utils.h"
#include "book/book_xml_table_handler.h"
#include "book/book_xml_heading_handler.h"
#include "book/book_xml_image_handler.h"
#include "book/book_xml_anchor_handler.h"
#include "book/book_xml_element_style.h"
#include "book/book_xml_flow_emission.h"
#include "book/book_xml_inline_handler.h"
#include "book/book_xml_block_handler.h"
#include "book/book_xml_screen_advance.h"
#include "book/book_xml_text_emit.h"
#include "book/book_xml.h"
#include "book/inline_image_layout.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/html_entity_utils.h"
#include "reader/inline_link_utils.h"
#include "book/heading_layout.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_page_cache.h"
#include "shared/main.h"
#include "book/page.h"
#include "book/page_buffer_utils.h"
#include "shared/debug_log.h"
#include "parse.h"
#include "book/reflow_cache_save_utils.h"
#include "settings/prefs.h"
#include "ui/screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_render_layout_utils.h"
#include "shared/text_bidi_utils.h"
#include "shared/text_unicode_utils.h"
#include "shared/string_utils.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

namespace {

static const size_t kFb2BinaryMaxChars = 6 * 1024 * 1024;
constexpr int kCompactBottomMargin = 20;

using book_xml_css_style_utils::ResolveHorizontalMarginPx;

static bool EqualsAsciiNoCase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a;
    unsigned char cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z')
      ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z')
      cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb)
      return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static std::string ToLowerAsciiLocal(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static void AppendParsedByte(parsedata_t *p, u32 c) {
  parse_append_page_byte(p, c);
}

static bool HasVisibleTextContentUtf8(const char *txt, int txtlen) {
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

static bool ParseInAnyEasyParagraphTightBlock(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (book_xml_block_utils::SuppressInnerParagraphSpacing(p->stack[i]))
      return true;
  }
  return false;
}

static void QueueDeferredStyleSync(parsedata_t *p, bool want_bold,
                                   bool want_italic, bool want_underline,
                                   u8 want_underline_style,
                                   bool want_overline,
                                   bool want_strikethrough,
                                   bool want_superscript, bool want_subscript,
                                   bool want_mono) {
  book_xml_inline_state::QueueDeferredStyleSync(
      p, want_bold, want_italic, want_underline, want_underline_style,
      want_overline, want_strikethrough, want_superscript, want_subscript,
      want_mono);
}

static bool ContainsAsciiNoCase(const std::string &haystack,
                                const char *needle) {
  if (!needle || !needle[0])
    return false;
  const std::string haystack_lc = ToLowerAsciiLocal(haystack);
  const std::string needle_lc = ToLowerAsciiLocal(needle);
  return haystack_lc.find(needle_lc) != std::string::npos;
}

static bool ClassListContains(const char *class_attr, const char *needle) {
  if (!class_attr || !needle || !needle[0])
    return false;

  const std::string classes = ToLowerAsciiLocal(class_attr);
  const std::string target = ToLowerAsciiLocal(needle);
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

static bool AttrNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopPx(const char **attr) {
  return book_xml_css_resolver::ParseElementMarginTopPx(attr);
}

// Return the author-specified max width in pixels for an img element.
// Checks the HTML width attribute first, then the CSS width property in style.
// Returns 0 if no usable constraint found.
static int ParseImgWidthPx(const char *width_attr, const char *style,
                            int text_width, int font_px) {
  return book_xml_css_resolver::ParseImgWidthPx(width_attr, style, text_width,
                                                 font_px);
}

static std::string ExtractStyleAttr(const char **attr) {
  return book_xml_css_resolver::ExtractStyleAttr(attr);
}

static std::string ExtractClassAttr(const char **attr) {
  return book_xml_css_resolver::ExtractClassAttr(attr);
}



static void AlignFreshLineToBlockMargin(parsedata_t *p, Text *ts) {
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

static book_xml_css_style_utils::WhiteSpaceMode
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

static book_xml_css_style_utils::MarginTopResult
ParseElementMarginBottomWithClass(const std::string &last_style,
                                  const std::string &last_class,
                                  const epub_css_class_map::CssClassMap &class_map,
                                  const char *element_tag = nullptr) {
  return book_xml_css_resolver::ParseElementMarginBottomWithClass(
      last_style, last_class, class_map, element_tag);
}

static book_xml_css_style_utils::TextAlign
ResolveElementTextAlignWithClass(const std::string &style_attr,
                                 const std::string &class_attr,
                                 const parsedata_t *p,
                                 const epub_css_class_map::CssClassMap &class_map,
                                 const char *element_tag = nullptr) {
  if (!p)
    return book_xml_css_style_utils::TextAlign::Left;
  return book_xml_css_resolver::ResolveElementTextAlignWithClass(
      style_attr, class_attr, p->block_text_align_stack,
      p->block_text_align_value_stack, p->stacksize, class_map, element_tag);
}

static void AppendParagraphAlignMarker(
    parsedata_t *p, book_xml_css_style_utils::TextAlign align) {
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

static bool ElementCanCarryBlockTextAlign(const char *el,
                                          const std::string &style_attr) {
  return book_xml_css_resolver::ElementCanCarryBlockTextAlign(el, style_attr);
}

static void RestoreActiveBlockTextAlignMarker(parsedata_t *p) {
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

static bool ImagePathLooksLikeSvgWrapper(const std::string &path) {
  static const std::vector<u8> empty;
  return epub_image_utils::LooksLikeSvgWrapper(path, empty);
}

static void LogResolvedBlockMargin(parsedata_t *p, const char *tag,
                                   const char *phase,
                                   const std::string &style_attr,
                                   const std::string &class_attr,
                                   const book_xml_css_style_utils::MarginTopResult &m,
                                   int line_h, int default_lf, int final_lf) {
  (void)p; (void)tag; (void)phase; (void)style_attr;
  (void)class_attr; (void)m; (void)line_h; (void)default_lf; (void)final_lf;
}



static void ParseElementStyleFlags(const char **attr, bool *bold_out,
                                   bool *italic_out, bool *underline_out,
                                   u8 *underline_style_out,
                                   bool *overline_out, bool *strikethrough_out,
                                   bool *superscript_out, bool *subscript_out,
                                   bool *no_underline_out,
                                   bool *reset_bold_out,
                                   bool *reset_italic_out) {
  return book_xml_css_resolver::ParseElementStyleFlags(
      attr, bold_out, italic_out, underline_out,
      reinterpret_cast<uint8_t *>(underline_style_out),
      overline_out, strikethrough_out, superscript_out, subscript_out,
      no_underline_out, reset_bold_out, reset_italic_out);
}

static void ParseElementHiddenFlags(const char **attr, bool *hidden_out) {
  return book_xml_css_resolver::ParseElementHiddenFlags(attr, hidden_out);
}

static bool HasActiveStackBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackBoldStyle(p);
}
static bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackHiddenStyle(p);
}
static bool HasActiveStackItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackItalicStyle(p);
}
static bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackUnderlineStyle(p);
}
static u8 ResolveActiveUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::ResolveActiveUnderlineStyle(p);
}
static bool HasActiveStackOverlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackOverlineStyle(p);
}
static bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackStrikethroughStyle(p);
}
static bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSuperscriptStyle(p);
}
static bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackSubscriptStyle(p);
}
static bool HasActiveStackNoUnderlineStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackNoUnderlineStyle(p);
}
static bool HasActiveStackResetBoldStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetBoldStyle(p);
}
static bool HasActiveStackResetItalicStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackResetItalicStyle(p);
}
static bool HasActiveStackMonoStyle(const parsedata_t *p) {
  return book_xml_inline_state::HasActiveStackMonoStyle(p);
}

#ifdef DSLIBRIS_DEBUG
struct ChardataPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit ChardataPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~ChardataPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_chardata_calls++;
    parsedata->perf_chardata_ms += (u64)(osGetTime() - t_begin);
  }
};

struct ElementPerfScope {
  parsedata_t *parsedata;
  u64 t_begin;

  explicit ElementPerfScope(parsedata_t *parsedata_)
      : parsedata(parsedata_), t_begin(osGetTime()) {}

  ~ElementPerfScope() {
    if (!parsedata)
      return;
    parsedata->perf_element_calls++;
    parsedata->perf_element_ms += (u64)(osGetTime() - t_begin);
  }
};

#else
struct ChardataPerfScope {
  explicit ChardataPerfScope(parsedata_t *) {}
};
struct ElementPerfScope {
  explicit ElementPerfScope(parsedata_t *) {}
};
#endif

// Thin wrappers for book_xml_screen_advance public functions — preserves all
// existing call signatures so zero call-site changes are needed in this file.
static void linefeed(parsedata_t *p) { book_xml_screen_advance::Linefeed(p); }
static void linefeed_r(parsedata_t *p, const char *, const char *, int) {
  book_xml_screen_advance::Linefeed(p);
}
static bool blankline(parsedata_t *p) { return book_xml_screen_advance::Blankline(p); }
static void ApplyClearBreak(parsedata_t *p) { book_xml_screen_advance::ApplyClearBreak(p); }
static void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  book_xml_inline_state::RestoreParsedInlineLinkMarker(p);
}
static bool IsCurrentReadingScreenVisuallyEmpty(const parsedata_t *p) {
  return book_xml_screen_advance::IsCurrentReadingScreenVisuallyEmpty(p);
}
static void ClearPendingBlockSpacing(parsedata_t *p) {
  book_xml_screen_advance::ClearPendingBlockSpacing(p);
}
static void AdvanceParsedPageOnOverflow(parsedata_t *p, int lh) {
  book_xml_screen_advance::AdvanceParsedPageOnOverflow(p, lh);
}
static void AdvanceParsedScreen(parsedata_t *p) {
  book_xml_screen_advance::AdvanceParsedScreen(p);
}
static void ForcePageBreak(parsedata_t *p) {
  book_xml_screen_advance::ForcePageBreak(p);
}
static void QueueBlockSpacingLines(parsedata_t *p, int lines, const char *tag,
                                   const char *reason, bool from_css) {
  book_xml_screen_advance::QueueBlockSpacingLines(p, lines, tag, reason, from_css);
}
static void SuppressPendingBlockSpacingFromCss(parsedata_t *p, const char *tag,
                                               const char *reason) {
  book_xml_screen_advance::SuppressPendingBlockSpacingFromCss(p, tag, reason);
}
static void QueueBlockSpacingFromMarginResult(
    parsedata_t *p, const char *tag, const char *reason,
    const book_xml_css_style_utils::MarginTopResult &mtr,
    int line_h, int default_lf) {
  book_xml_screen_advance::QueueBlockSpacingFromMarginResult(
      p, tag, reason, mtr, line_h, default_lf);
}
static void FlushPendingBlockSpacingBeforeContent(parsedata_t *p,
                                                  const char *tag) {
  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, tag);
}

// Adapters for the FlowEmissionFns callback struct.
static void FlowAdvanceScreen(parsedata_t *p) {
  book_xml_screen_advance::AdvanceParsedScreen(p);
}
static void FlowAdvancePageOverflow(parsedata_t *p, int lh) {
  book_xml_screen_advance::AdvanceParsedPageOnOverflow(p, lh);
}
static void FlowFlushPendingBlock(parsedata_t *p, const char *tag) {
  book_xml_screen_advance::FlushPendingBlockSpacingBeforeContent(p, tag);
}
static FlowEmissionFns MakeFlowEmissionFns() {
  FlowEmissionFns f;
  f.advance_screen = FlowAdvanceScreen;
  f.advance_page_overflow = FlowAdvancePageOverflow;
  f.flush_pending_block = FlowFlushPendingBlock;
  return f;
}

// Thin wrappers for flow emission — zero call-site changes required.
static void SyncParsedTextStyle(Text *ts, bool bold, bool italic, bool mono) {
  if (!ts)
    return;
  ts->SetStyle(
      book_xml_parser_style_utils::ResolveParsedTextStyle(bold, italic, mono));
}
static void ApplyDeferredStyleSync(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::ApplyDeferredStyleSync(p, ts);
}
static void FlushInlineTailAndDeferredStyle(parsedata_t *p, Text *ts) {
  book_xml_flow_emission::FlushInlineTailAndDeferredStyle(p, ts, MakeFlowEmissionFns());
}
static void QueueFlowedFragmentRaw(parsedata_t *p, const XML_Char *txt,
                                   int txtlen) {
  book_xml_flow_emission::QueueFlowedFragmentRaw(p, txt, txtlen, MakeFlowEmissionFns());
}

static std::string NormalizeDocPath(const std::string &path) {
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

static bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle) || EqualsAsciiNoCase(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && (strcmp(colon + 1, needle) == 0 ||
                    EqualsAsciiNoCase(colon + 1, needle)));
}

static bool PathLooksLikeTocDoc(const std::string &path) {
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

static bool DocLooksLikeTocDoc(const parsedata_t *p) {
  if (!p)
    return false;
  return PathLooksLikeTocDoc(p->docpath) || PathLooksLikeTocDoc(p->doc_title) ||
         PathLooksLikeTocDoc(p->doc_heading);
}

static std::string ResolveDocPath(const std::string &base_doc_path,
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

static std::string NormalizeFb2ChapterTitle(const std::string &in) {
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


static bool IsBlockLevelElement(const char *el) {
  return !strcmp(el, "p") || !strcmp(el, "div") ||
         !strcmp(el, "h1") || !strcmp(el, "h2") || !strcmp(el, "h3") ||
         !strcmp(el, "h4") || !strcmp(el, "h5") || !strcmp(el, "h6") ||
         !strcmp(el, "section") || !strcmp(el, "article") ||
         !strcmp(el, "aside") || !strcmp(el, "blockquote") ||
         !strcmp(el, "header") || !strcmp(el, "footer") ||
         !strcmp(el, "figure") || !strcmp(el, "dl") ||
         !strcmp(el, "dt") || !strcmp(el, "dd");
}

// True if element is a native block element OR promoted to block via CSS.
static bool BehavesAsBlock(const char *el,
                           const epub_css_class_map::CssClassMargins &elem_css) {
  return IsBlockLevelElement(el) || elem_css.is_display_block;
}

static bool IsFigureContainerElement(const char *el, const char *class_attr) {
  if (!el)
    return false;
  if (!strcmp(el, "figure"))
    return true;
  return !strcmp(el, "div") && ClassListContains(class_attr, "figure");
}

static void FlushInlineTailBeforeElementStart(parsedata_t *p, Text *ts,
                                              const char *el) {
  if (!p)
    return;

  // Whitespace-only indentation/newlines between block elements must not be
  // emitted as real flowed text. If emitted, it can consume pending block
  // breaks before the next real block content arrives.
  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

static void FlushInlineTailBeforeElementEnd(parsedata_t *p, Text *ts,
                                            const char *el) {
  if (!p)
    return;

  // Whitespace-only indentation/newlines before closing block elements must not
  // be emitted as real flowed text. Otherwise XHTML pretty-printing between
  // </p>, </dd>, </dl>, etc. can affect line state and consume pending breaks.
  if (el && IsBlockLevelElement(el) && !p->inline_text_tail.empty() &&
      !HasVisibleTextContentUtf8(p->inline_text_tail.c_str(),
                                 (int)p->inline_text_tail.size())) {
    p->inline_text_tail.clear();
    ApplyDeferredStyleSync(p, ts);
    return;
  }

  FlushInlineTailAndDeferredStyle(p, ts);
}

static void SetCurrentStackHidden(parsedata_t *p, bool hidden) {
  if (!p || p->stacksize == 0)
    return;
  p->style_hidden_stack[p->stacksize - 1] = hidden;
}

} // namespace

namespace xml {
namespace book {
namespace metadata {

std::string title;

void start(void *userdata, const char *el, const char **attr) {
  //! Expat callback, when entering an element.
  //! For finding book title only.

  if (!strcmp(el, "title")) {
    parse_push((parsedata_t *)userdata, TAG_TITLE);
  }
}

void chardata(void *userdata, const char *txt, int txtlen) {
  //! Expat callback, when in char data for element.
  //! For finding book title only.

  if (!parse_in((parsedata_t *)userdata, TAG_TITLE))
    return;
  title = txt;
}

void end(void *userdata, const char *el) {
  //! Expat callback, when exiting an element.
  //! For finding book title only.

  parsedata_t *data = (parsedata_t *)userdata;
  if (!strcmp(el, "title"))
    data->book->SetTitle(title.c_str());
  if (!strcmp(el, "head"))
    data->status = 1; // done.
  parse_pop(data);
}

} // namespace metadata
} // namespace book
} // namespace xml

namespace xml {
namespace book {

void chardata(void *data, const XML_Char *txt, int txtlen);

void instruction(void *data, const char *target, const char *pidata) {}

// Adapters bridging the anonymous-namespace statics into the TableHandlerFns
// callback struct so book_xml_table_handler.cpp can call them.
static void TableLf(parsedata_t *p) { linefeed(p); }
static void TableFlush(parsedata_t *p, Text *ts) {
  FlushInlineTailAndDeferredStyle(p, ts);
}
static void TableEmit(parsedata_t *p, const char *txt, int len) {
  book_xml_flow_emission::EmitFlowedFragmentRaw(p, txt, len, MakeFlowEmissionFns());
}
static TableHandlerFns MakeTableHandlerFns() {
  TableHandlerFns f;
  f.linefeed = TableLf;
  f.flush_inline_tail = TableFlush;
  f.emit_flowed_raw = TableEmit;
  return f;
}

// Pre-resolved CSS overloads — accept a single LookupAllForClassAttr result
// instead of re-scanning the class map for each property.

static book_xml_css_style_utils::ClearMode
ParseElementClear(const char **attr,
                  const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementClear(attr, elem_css);
}
static book_xml_css_style_utils::FloatMode
ParseElementFloat(const char **attr,
                  const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementFloat(attr, elem_css);
}
static u8 ParseElementTextTransform(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementTextTransform(attr, elem_css);
}
static u8 ParseElementWhiteSpace(
    const char **attr, const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementWhiteSpace(attr, elem_css);
}
static void ApplyElementBlockMargins(
    parsedata_t *p, Text *ts, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  book_xml_element_style::ApplyElementBlockMargins(p, ts, attr, elem_css);
}
static book_xml_css_style_utils::MarginTopResult
ParseElementMarginTopWithClass(const char **attr,
                               const epub_css_class_map::CssClassMargins &elem_css) {
  return book_xml_element_style::ParseElementMarginTopWithClass(attr, elem_css);
}
static void ConfigureBlockTextAlign(
    parsedata_t *p, const char *el, const char **attr,
    const epub_css_class_map::CssClassMargins &elem_css) {
  book_xml_element_style::ConfigureBlockTextAlign(p, el, attr, elem_css);
}
static void EnsureBlockBoundaryBeforeBlockStart(parsedata_t *p,
                                                const char *tag,
                                                const char *reason) {
  book_xml_element_style::EnsureBlockBoundaryBeforeBlockStart(p, tag, reason);
}

static void HeadingEnsureBlockBoundary(parsedata_t *p, const char *tag,
                                       const char *phase) {
  EnsureBlockBoundaryBeforeBlockStart(p, tag, phase);
}
static void HeadingAdvanceScreen(parsedata_t *p) { AdvanceParsedScreen(p); }
static void HeadingQueueBlockSpacing(
    parsedata_t *p, const char *tag, const char *phase,
    const book_xml_css_style_utils::MarginTopResult &mtr, int line_h,
    int default_lf) {
  QueueBlockSpacingFromMarginResult(p, tag, phase, mtr, line_h, default_lf);
}
static HeadingHandlerFns MakeHeadingHandlerFns() {
  HeadingHandlerFns f;
  f.ensure_block_boundary = HeadingEnsureBlockBoundary;
  f.advance_screen = HeadingAdvanceScreen;
  f.queue_block_spacing = HeadingQueueBlockSpacing;
  return f;
}

static void AnchorLf(parsedata_t *p) { linefeed(p); }
static AnchorHandlerFns MakeAnchorHandlerFns() {
  AnchorHandlerFns f;
  f.linefeed = AnchorLf;
  return f;
}

static void ImageLf(parsedata_t *p) { linefeed(p); }
static void ImageAdvanceScreen(parsedata_t *p) { AdvanceParsedScreen(p); }
static void ImageAdvancePageOverflow(parsedata_t *p, int lineheight) {
  AdvanceParsedPageOnOverflow(p, lineheight);
}
static void ImageEmitChardata(parsedata_t *p, const char *txt, int len) {
  chardata((void *)p, txt, len);
}
static ImageHandlerFns MakeImageHandlerFns() {
  ImageHandlerFns f;
  f.linefeed = ImageLf;
  f.advance_screen = ImageAdvanceScreen;
  f.advance_page_overflow = ImageAdvancePageOverflow;
  f.emit_chardata = ImageEmitChardata;
  return f;
}

// Forward declaration — chardata is defined later in this namespace.
void chardata(void *data, const XML_Char *txt, int txtlen);

static void InlineEmitChardata(parsedata_t *p, const char *txt, int len) {
  chardata(p, txt, len);
}
static InlineHandlerFns MakeInlineHandlerFns() {
  InlineHandlerFns f;
  f.emit_chardata = InlineEmitChardata;
  return f;
}

void start(void *data, const char *el, const char **attr) {
  //! Expat callback, when starting an element.

  parsedata_t *p = (parsedata_t *)data;
  ElementPerfScope elem_perf(p);
  Text *ts = p->ts;
  FlushInlineTailBeforeElementStart(p, ts, el);

  if (book_xml_hidden_utils::IsCosmeticPageBreakElement(attr)) {
    parse_push(p, TAG_UNKNOWN);
    SetCurrentStackHidden(p, true);
    return;
  }

  if (p->fb2_mode && parse_in(p, TAG_BODY)) {
    if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth < 31)
        p->fb2_section_depth++;
      if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
        p->fb2_section_has_chapter[p->fb2_section_depth] = false;
    } else if (XmlNameEquals(el, "title") && p->fb2_section_depth > 0) {
      p->fb2_title_depth++;
      if (p->fb2_title_depth == 1 && p->fb2_title_capture_depth == 0 &&
          p->fb2_section_depth < 32 &&
          !p->fb2_section_has_chapter[p->fb2_section_depth]) {
        p->fb2_title_capture_depth = p->fb2_section_depth;
        p->fb2_title_text.clear();
      }
    }
  }

  // Register named anchors while parsing EPUB documents so TOC hrefs with
  // fragments (#id) can jump to the closest real page instead of chapter start.
  if (p->book && !p->docpath.empty() && attr) {
    for (int i = 0; attr[i]; i += 2) {
      if (!attr[i + 1] || !attr[i + 1][0])
        continue;
      if (XmlNameEquals(attr[i], "id") || XmlNameEquals(attr[i], "name")) {
        p->book->AddChapterAnchor(p->docpath, attr[i + 1]);
      }
    }
  }

  if (HandleTableStart(p, ts, el, attr, MakeTableHandlerFns()))
    return;

  // One-shot CSS class resolution for this element — avoids 18+ separate
  // map lookups by resolving all CSS properties in a single pass.
  const char *el_class_raw = nullptr;
  const char *el_style_raw = nullptr;
  for (int i = 0; attr && attr[i]; i += 2) {
    if (AttrNameEquals(attr[i], "class"))
      el_class_raw = attr[i + 1];
    else if (AttrNameEquals(attr[i], "style"))
      el_style_raw = attr[i + 1];
  }
  epub_css_class_map::CssClassMargins elem_css;
  epub_css_class_map::LookupAllForTag(el, p->css_class_map, &elem_css);
  epub_css_class_map::MergeClassRulesToStyle(
      el_class_raw ? std::string(el_class_raw) : std::string(),
      p->css_class_map, &elem_css);

  if (BehavesAsBlock(el, elem_css)) {
    if (ParseElementClear(attr, elem_css) !=
        book_xml_css_style_utils::ClearMode::None) {
      ApplyClearBreak(p);
    }
    if (!IsFigureContainerElement(el, el_class_raw) &&
        (book_xml_css_style_utils::HasPageBreakInsideAvoid(el_style_raw) ||
         elem_css.page_break_inside_avoid) &&
        p->buflen > 0 && !blankline(p)) {
      ForcePageBreak(p);
    }
    if (book_xml_css_style_utils::HasPageBreakBefore(el_style_raw) ||
        elem_css.page_break_before) {
      ForcePageBreak(p);
    }
  }

  if (!strcmp(el, "h1")) {
    HandleHeadingStart(p, ts, attr, elem_css, 1, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h2")) {
    HandleHeadingStart(p, ts, attr, elem_css, 2, MakeHeadingHandlerFns());
  } else if (!strcmp(el, "h3")) {
    HandleHeadingStart(p, ts, attr, elem_css, 3, MakeHeadingHandlerFns());
  } else if (XmlNameEquals(el, "title")) {
    parse_push(p, TAG_TITLE);
  } else {
    bool block_early_return = false;
    if (book_xml_block_handler::HandleBlockElementStart(
            p, ts, el, attr, elem_css, el_class_raw, &block_early_return)) {
      if (block_early_return) return;
    } else if (book_xml_inline_handler::HandleNamedInlineElementStart(
                   p, ts, el, attr, MakeInlineHandlerFns())) {
      // handled
    } else if (!strcmp(el, "a")) {
      HandleAnchorStart(p, attr);
    } else if (XmlNameEquals(el, "img") || XmlNameEquals(el, "image")) {
      HandleInlineImageStart(p, ts, attr, elem_css, MakeImageHandlerFns());
    } else if (XmlNameEquals(el, "binary")) {
      parse_push(p, TAG_UNKNOWN);

      p->collecting_fb2_binary = false;
      p->fb2_binary_too_large = false;
      p->fb2_binary_id.clear();
      p->fb2_binary_data.clear();

      const char *id = NULL;
      for (int i = 0; attr && attr[i]; i += 2) {
        if (XmlNameEquals(attr[i], "id")) {
          id = attr[i + 1];
        }
      }

      if (id && *id && p->book) {
        p->collecting_fb2_binary = true;
        p->fb2_binary_id = id;
        if (!p->fb2_binary_id.empty() && p->fb2_binary_id[0] == '#')
          p->fb2_binary_id.erase(0, 1);
      }
    } else {
      parse_push(p, TAG_UNKNOWN);
    }
  }

  ConfigureBlockTextAlign(p, el, attr, elem_css);

  book_xml_block_handler::ApplyDisplayBlockPromotion(p, ts, el, attr, elem_css);

  book_xml_inline_handler::HandleCssInlineStylingStart(p, ts, el, attr, elem_css);

  if (BehavesAsBlock(el, elem_css) && p->stacksize > 0) {
    if (book_xml_css_style_utils::HasPageBreakAfter(el_style_raw) ||
        elem_css.page_break_after) {
      p->page_break_after_stack[p->stacksize - 1] = true;
    }
  }
}

void chardata(void *data, const XML_Char *txt, int txtlen) {
  //! reflow text on the fly, into page data structure.

  parsedata_t *p = (parsedata_t *)data;
  ChardataPerfScope perf_scope(p);

  if (p->collecting_fb2_binary) {
    if (!p->fb2_binary_too_large) {
      for (int i = 0; i < txtlen; i++) {
        unsigned char c = (unsigned char)txt[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          continue;
        p->fb2_binary_data.push_back((char)c);
      }
      if (p->fb2_binary_data.size() > kFb2BinaryMaxChars) {
        p->fb2_binary_data.clear();
        p->fb2_binary_too_large = true;
      }
    }
    return;
  }

  if (parse_in(p, TAG_TITLE)) {
    if (p->fb2_mode && p->fb2_title_capture_depth > 0) {
      p->fb2_title_text.append((const char *)txt, txtlen);
    } else {
      p->doc_title.append((const char *)txt, txtlen);
    }
    return;
  }
  if (parse_in(p, TAG_SCRIPT))
    return;
  if (parse_in(p, TAG_STYLE))
    return;
  if (HasActiveStackHiddenStyle(p))
    return;
  if (parse_in(p, TAG_TABLE)) {
    std::string *buffer = GetActiveCapturedTableText(p);
    if (buffer)
      buffer->append((const char *)txt, (size_t)txtlen);
    return;
  }
  if (!p->doc_heading_complete &&
      (parse_in(p, TAG_H1) || parse_in(p, TAG_H2) || parse_in(p, TAG_H3)) &&
      p->doc_heading.size() < 160) {
    p->doc_heading.append((const char *)txt, txtlen);
  }

  if (p->strip_leading_list_marker) {
    bool all_whitespace_only = false;
    size_t strip = text_unicode_utils::StripLeadingListMarkerUtf8(
        txt, (size_t)txtlen, &all_whitespace_only);
    if (strip > 0) {
      txt += strip;
      txtlen -= (int)strip;
      p->strip_leading_list_marker = false;
      if (txtlen <= 0)
        return;
    } else if (all_whitespace_only) {
      return;
    } else {
      p->strip_leading_list_marker = false;
    }
  }

  if (book_xml_list_utils::HasPendingListItemContent(p) &&
      HasVisibleTextContentUtf8(txt, txtlen)) {
    book_xml_list_utils::ConsumePendingListItemContent(p);
  }
  QueueFlowedFragmentRaw(p, txt, txtlen);
}

void end(void *data, const char *el) {
  parsedata_t *p = (parsedata_t *)data;
  ElementPerfScope elem_perf(p);
  Text *ts = p->ts;
  FlushInlineTailBeforeElementEnd(p, ts, el);

  if (XmlNameEquals(el, "binary")) {
    if (p->collecting_fb2_binary && !p->fb2_binary_too_large && p->book &&
        !p->fb2_binary_id.empty() && !p->fb2_binary_data.empty()) {
      p->book->StoreFb2InlineImage(p->fb2_binary_id, p->fb2_binary_data);
    }
    p->collecting_fb2_binary = false;
    p->fb2_binary_too_large = false;
    p->fb2_binary_id.clear();
    p->fb2_binary_data.clear();
    parse_pop(p);
    return;
  }

  if (p->fb2_mode) {
    if (XmlNameEquals(el, "title")) {
      if (p->fb2_title_depth > 0) {
        bool finishing_capture =
            (p->fb2_title_depth == 1 && p->fb2_title_capture_depth > 0 &&
             p->fb2_title_capture_depth == p->fb2_section_depth);
        if (finishing_capture && p->book) {
          std::string chapter_title =
              NormalizeFb2ChapterTitle(p->fb2_title_text);
          if (!chapter_title.empty()) {
            int level = p->fb2_section_depth > 0 ? p->fb2_section_depth - 1 : 0;
            if (level > 255)
              level = 255;
            p->book->AddChapter(p->book->GetPageCount(), chapter_title,
                                (u8)level);
            if (p->fb2_section_depth >= 0 && p->fb2_section_depth < 32)
              p->fb2_section_has_chapter[p->fb2_section_depth] = true;
          }
          p->fb2_title_text.clear();
          p->fb2_title_capture_depth = 0;
        }
        p->fb2_title_depth--;
        if (p->fb2_title_depth < 0)
          p->fb2_title_depth = 0;
      }
    } else if (XmlNameEquals(el, "section")) {
      if (p->fb2_section_depth > 0) {
        if (p->fb2_section_depth < 32)
          p->fb2_section_has_chapter[p->fb2_section_depth] = false;
        p->fb2_section_depth--;
      }
      if (p->fb2_section_depth < 0)
        p->fb2_section_depth = 0;
      if (p->fb2_title_capture_depth > p->fb2_section_depth) {
        p->fb2_title_capture_depth = 0;
        p->fb2_title_text.clear();
      }
    }
  }

  if (HandleTableEnd(p, ts, el, MakeTableHandlerFns()))
    return;

  if (!strcmp(el, "body")) {
    FlushInlineTailAndDeferredStyle(p, ts);
    // Save off our last page.
    Page *page = p->book->AppendPage();
    page->SetBuffer(p->buf, p->buflen);
    parse_reset_page_buffer(p);
    // Retain styles across the page.
    book_xml_parser_style_utils::RestoreParsedStyleMarkers(p);
    RestoreParsedInlineLinkMarker(p);
    parse_pop(p);
    return;
  }

  if (!strcmp(el, "a")) {
    HandleAnchorEnd(p, MakeAnchorHandlerFns());
  } else if (book_xml_block_handler::HandleBlockElementEnd(p, ts, el)) {
    // handled
  } else if (!IsBlockLevelElement(el) && p->stacksize > 0 &&
             p->block_text_align_stack[(u8)(p->stacksize - 1)]) {
    FlushInlineTailAndDeferredStyle(p, ts);
    if (p->linebegan)
      QueueBlockSpacingLines(p, 1, el, "block-align-bottom", false);
    p->block_margin_left = 0;
    p->block_margin_right = 0;
  }

  bool restore_block_text_align = false;
  bool had_page_break_after = false;
  u8 restore_font_size_px = 0;
  if (p->stacksize > 0) {
    const u8 current = (u8)(p->stacksize - 1);
    restore_block_text_align = p->block_text_align_stack[current];
    if (IsBlockLevelElement(el) || p->page_break_after_stack[current])
      had_page_break_after = p->page_break_after_stack[current];
    restore_font_size_px = p->style_font_size_restore_stack[current];
    // Emit closing paren for <rt> at the reduced annotation size, before
    // font restore fires below.
    if (p->stack[current] == TAG_RT && !HasActiveStackHiddenStyle(p))
      chardata(p, ")", 1);
  }

  parse_pop(p);
  if (restore_block_text_align)
    RestoreActiveBlockTextAlignMarker(p);
  if (had_page_break_after)
    ForcePageBreak(p);
  if (restore_font_size_px) {
    ts->SetPixelSize(restore_font_size_px);
    AppendParsedByte(p, TEXT_FONT_SIZE);
    AppendParsedByte(p, restore_font_size_px);
  }

  book_xml_inline_handler::SyncInlineStyleAfterPop(p, ts);
}

int unknown(void *encodingHandlerData, const XML_Char *name,
            XML_Encoding *info) {
  return 0;
}

void fallback(void *data, const XML_Char *s, int len) {
  parsedata_t *p = (parsedata_t *)data;
  if (!p || !s || len <= 0 || s[0] != '&')
    return;

  uint32_t cp = 0;
  if (!html_entity_utils::DecodeHtmlEntityCodepoint(std::string(s, len), &cp))
    return;

  FlushInlineTailAndDeferredStyle(p, p->ts);
  AppendParsedByte(p, (u32)cp);
  p->pen.x += p->ts->GetAdvance(cp);
}

} // namespace book
} // namespace xml
