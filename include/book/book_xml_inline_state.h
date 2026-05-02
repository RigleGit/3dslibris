#pragma once

#include "parse.h"
#include "shared/text_token_constants.h"

namespace book_xml_inline_state {

inline bool HasActiveStackBoldStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_bold_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackItalicStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_italic_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_underline_stack[i])
      return true;
  }
  return false;
}

inline u8 ResolveActiveUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return UNDERLINE_STYLE_SOLID;
  for (int i = (int)p->stacksize - 1; i >= 0; i--) {
    if (p->style_underline_stack[i])
      return p->style_underline_style_stack[i];
  }
  for (int i = (int)p->stacksize - 1; i >= 0; i--) {
    if (p->stack[i] == TAG_UNDERLINE)
      return UNDERLINE_STYLE_SOLID;
  }
  return UNDERLINE_STYLE_SOLID;
}

inline bool HasActiveStackOverlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_overline_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackStrikethroughStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_strikethrough_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackSuperscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_superscript_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackSubscriptStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_subscript_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackNoUnderlineStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_no_underline_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackResetBoldStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_reset_bold_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackResetItalicStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_reset_italic_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackMonoStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_mono_stack[i])
      return true;
  }
  return false;
}

inline bool HasActiveStackHiddenStyle(const parsedata_t *p) {
  if (!p)
    return false;
  for (u8 i = 0; i < p->stacksize; i++) {
    if (p->style_hidden_stack[i])
      return true;
  }
  return false;
}

inline bool GetTopActiveInlineLink(const parsedata_t *p, u16 *href_id_out) {
  if (!p || !href_id_out)
    return false;
  for (int i = (int)p->stacksize - 1; i >= 0; --i) {
    if (!p->link_active_stack[i])
      continue;
    if (p->link_href_id_stack[i] == 0)
      continue;
    *href_id_out = p->link_href_id_stack[i];
    return true;
  }
  return false;
}

inline void RestoreParsedInlineLinkMarker(parsedata_t *p) {
  if (!p)
    return;
  u16 href_id = 0;
  if (!GetTopActiveInlineLink(p, &href_id))
    return;
  parse_append_page_byte(p, TEXT_LINK_START);
  parse_append_page_byte(p, (u32)href_id);
}

inline void QueueDeferredStyleSync(parsedata_t *p, bool want_bold,
                                   bool want_italic, bool want_underline,
                                   u8 want_underline_style, bool want_overline,
                                   bool want_strikethrough,
                                   bool want_superscript, bool want_subscript,
                                   bool want_mono) {
  if (!p)
    return;
  p->deferred_style_sync = true;
  p->deferred_target_bold = want_bold;
  p->deferred_target_italic = want_italic;
  p->deferred_target_underline = want_underline;
  p->deferred_target_underline_style = want_underline_style;
  p->deferred_target_overline = want_overline;
  p->deferred_target_strikethrough = want_strikethrough;
  p->deferred_target_superscript = want_superscript;
  p->deferred_target_subscript = want_subscript;
  p->deferred_target_mono = want_mono;
}

} // namespace book_xml_inline_state
