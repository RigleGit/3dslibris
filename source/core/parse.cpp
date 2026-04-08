/*

dslibris - an ebook reader for the Nintendo DS.

 Copyright (C) 2007-2025 Yoyodyne Research, ZLC

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*/

/*
  3DS port modifications by Rigle (summary):
  - Extended parser state for multi-format book flows (EPUB/FB2/TXT/RTF/ODT).
  - Added compatibility helpers used by index/TOC heuristics.
  - Kept legacy parser behavior for dslibris content model compatibility.
*/

#include "parse.h"

#include <stdio.h>
#include <string.h>

bool iswhitespace(u32 c) {
  switch (c) {
  case ' ':
  case '\t':
  case '\n':
    return true;
  default:
    return false;
  }
}

void parse_reset_page_buffer(parsedata_t *data) {
  if (!data)
    return;
  memset(data->buf, 0, PAGEBUFSIZE * sizeof(u32));
  data->buflen = 0;
  data->pagebuf_overflowed = false;
  data->pagebuf_overflow_bytes = 0;
}

static void parse_note_page_buffer_overflow(parsedata_t *data, size_t bytes) {
  if (!data || bytes == 0)
    return;
  data->pagebuf_overflowed = true;
  data->pagebuf_overflow_bytes += bytes;
}

bool parse_append_page_byte(parsedata_t *data, u32 c) {
  if (!data)
    return false;
  if (data->buflen >= PAGEBUFSIZE) {
    parse_note_page_buffer_overflow(data, 1);
    return false;
  }
  data->buf[data->buflen++] = c;
  return true;
}

bool parse_append_page_byte_soft(parsedata_t *data, u32 c,
                                 parse_page_flush_fn flush_page, void *ctx) {
  if (!data)
    return false;
  if (parse_append_page_byte(data, c))
    return true;
  if (!flush_page || data->buflen <= 0)
    return false;
  if (!flush_page(data, ctx))
    return false;
  return parse_append_page_byte(data, c);
}

size_t parse_append_page_bytes(parsedata_t *data, const u32 *src, size_t len) {
  if (!data || !src || len == 0)
    return 0;
  if (data->buflen >= PAGEBUFSIZE) {
    parse_note_page_buffer_overflow(data, len);
    return 0;
  }

  const size_t available = (size_t)PAGEBUFSIZE - (size_t)data->buflen;
  const size_t written = (len < available) ? len : available;
  memcpy(data->buf + data->buflen, src, written * sizeof(u32));
  data->buflen += (int)written;
  if (written < len)
    parse_note_page_buffer_overflow(data, len - written);
  return written;
}

size_t parse_append_page_bytes_soft(parsedata_t *data, const u32 *src,
                                    size_t len,
                                    parse_page_flush_fn flush_page, void *ctx) {
  if (!data || !src || len == 0)
    return 0;

  const size_t available = (data->buflen >= PAGEBUFSIZE)
                               ? 0
                               : (size_t)PAGEBUFSIZE - (size_t)data->buflen;
  if (len <= available)
    return parse_append_page_bytes(data, src, len);

  if (!flush_page || data->buflen <= 0 || len > PAGEBUFSIZE)
    return parse_append_page_bytes(data, src, len);

  if (!flush_page(data, ctx))
    return parse_append_page_bytes(data, src, len);

  return parse_append_page_bytes(data, src, len);
}

bool parse_page_buffer_overflowed(const parsedata_t *data) {
  return data && data->pagebuf_overflowed;
}

void parse_init(parsedata_t *data) {
  data->stacksize = 0;
  data->pos = 0;
  data->reporter = NULL;
  data->ts = NULL;
  data->book = NULL;
  data->prefs = NULL;
  data->screen = 0;
  data->pen.x = 0;
  data->pen.y = 0;
  data->linebegan = false;
  data->preformatted_wrap_enabled = false;
  data->strip_leading_list_marker = false;
  data->in_paragraph = false;
  data->paragraph_has_content = false;
  data->bold = false;
  data->italic = false;
  data->underline = false;
  data->strikethrough = false;
  data->superscript = false;
  data->subscript = false;
  for (int i = 0; i < 32; i++) {
    data->style_bold_stack[i] = false;
    data->style_italic_stack[i] = false;
    data->style_underline_stack[i] = false;
    data->style_strikethrough_stack[i] = false;
    data->style_superscript_stack[i] = false;
    data->style_subscript_stack[i] = false;
    data->style_hidden_stack[i] = false;
  }
  data->docpath.clear();
  data->doc_title.clear();
  data->doc_heading.clear();
  data->collecting_fb2_binary = false;
  data->fb2_binary_too_large = false;
  data->fb2_binary_id.clear();
  data->fb2_binary_data.clear();
  data->fb2_mode = false;
  data->fb2_section_depth = 0;
  data->fb2_title_depth = 0;
  data->fb2_title_capture_depth = 0;
  for (int i = 0; i < 32; i++)
    data->fb2_section_has_chapter[i] = false;
  data->fb2_title_text.clear();
  data->perf_chardata_ms = 0;
  data->perf_chardata_calls = 0;
  data->perf_inline_images = 0;
  data->perf_page_overflows = 0;
  parse_reset_page_buffer(data);
  data->status = 0;
  data->pagecount = 0;
}

void parse_error(XML_Parser p, char *msg) {
  snprintf(msg, 128, "%d:%d: %s\n", (int)XML_GetCurrentLineNumber(p),
           (int)XML_GetCurrentColumnNumber(p),
           XML_ErrorString(XML_GetErrorCode(p)));
}

void parse_push(parsedata_t *data, context_t context) {
  if (data->stacksize < 32) {
    data->stack[data->stacksize] = context;
    data->style_bold_stack[data->stacksize] = false;
    data->style_italic_stack[data->stacksize] = false;
    data->style_underline_stack[data->stacksize] = false;
    data->style_strikethrough_stack[data->stacksize] = false;
    data->style_superscript_stack[data->stacksize] = false;
    data->style_subscript_stack[data->stacksize] = false;
    data->style_hidden_stack[data->stacksize] = false;
    data->stacksize++;
  }
}

context_t parse_pop(parsedata_t *data) {
  if (data->stacksize) {
    data->stacksize--;
    data->style_bold_stack[data->stacksize] = false;
    data->style_italic_stack[data->stacksize] = false;
    data->style_underline_stack[data->stacksize] = false;
    data->style_strikethrough_stack[data->stacksize] = false;
    data->style_superscript_stack[data->stacksize] = false;
    data->style_subscript_stack[data->stacksize] = false;
    data->style_hidden_stack[data->stacksize] = false;
  }
  return data->stack[data->stacksize];
}

bool parse_in(parsedata_t *data, context_t context) {
  u8 i;
  for (i = 0; i < data->stacksize; i++) {
    if (data->stack[i] == context)
      return true;
  }
  return false;
}
