/*
    3dslibris - plain_text_stream.cpp

    Incremental plain text pagination stream helpers.
*/

#include "formats/common/plain_text_stream.h"

#include "book/book_xml.h"
#include "formats/mobi/mobi_heading_markers.h"
#include "shared/text_token_constants.h"

namespace plain_text_stream {

using plain_text_perf_utils::CapturePlainTextPerfBaseline;
using plain_text_perf_utils::FillPlainTextStreamPerf;
using plain_text_perf_utils::PlainTextPerfBaseline;
using plain_text_perf_utils::PlainTextStreamPerf;

static PlainLineChunk ReadNextLineChunk(const std::string &text, size_t *cursor) {
  PlainLineChunk out = {nullptr, 0, false, false};
  if (!cursor || *cursor > text.size())
    return out;

  const size_t start = *cursor;
  const size_t end = text.find('\n', start);
  out.data = text.data() + start;
  if (end == std::string::npos) {
    out.size = text.size() - start;
    out.has_newline = false;
    *cursor = text.size() + 1;
  } else {
    out.size = end - start;
    out.has_newline = true;
    *cursor = end + 1;
  }
  out.valid = true;
  return out;
}

void InitState(State *state, const parsedata_t &base_parsedata,
               const std::string &text_utf8, bool detect_heuristic_headings) {
  if (!state)
    return;
  state->parsedata = base_parsedata;
  state->cursor = 0;
  state->prev_blank = true;
  state->prev_candidate = false;
  state->detect_heuristic_headings = detect_heuristic_headings;
  state->initialized = false;
  state->completed = false;
  state->text_bytes_fed = 0;
  state->curr = {nullptr, 0, false, false};
  state->next = {nullptr, 0, false, false};
  parse_push(&state->parsedata, TAG_PRE);

  state->curr = ReadNextLineChunk(text_utf8, &state->cursor);
  state->next = ReadNextLineChunk(text_utf8, &state->cursor);
  state->initialized = true;
}

bool ContinueState(State *state, const std::string &text_utf8, u32 budget_ms,
                   u16 page_budget, u16 min_pages_before_stop,
                   std::vector<u32> *text_cursor_per_page,
                   PlainTextStreamPerf *perf_out,
                   const ContinueCallbacks &callbacks) {
  if (!state || !state->initialized || state->completed)
    return true;
  if (!callbacks.is_blank_line || !callbacks.looks_like_plain_chapter_heading ||
      !callbacks.should_accept_heuristic_heading ||
      !callbacks.add_chapter_if_unique || !callbacks.apply_heading_keep_with_next ||
      !callbacks.append_inline_image_to_parsedata || !callbacks.finalize_plain_page ||
      !callbacks.set_non_epub_toc_confidence) {
    return true;
  }

  const u64 t_begin = osGetTime();
  const u16 page_start = state->parsedata.book->GetPageCount();
  u16 last_page_count = state->parsedata.book->GetPageCount();
  const size_t bytes_before_stream = state->text_bytes_fed;
  PlainTextPerfBaseline perf_baseline;
  CapturePlainTextPerfBaseline(state->parsedata, &perf_baseline);

  while (state->curr.valid) {
    bool curr_blank = false;
    bool next_blank = false;
    bool curr_candidate = false;
    const bool heuristic_headings = state->detect_heuristic_headings;
    bool curr_keep_with_next = false;
    if (heuristic_headings) {
      // Construct temporary strings only for the heuristic heading path (TXT
      // format). For MOBI, detect_heuristic_headings is false so this is skipped.
      static std::string curr_str;
      curr_str.assign(state->curr.data, state->curr.size);
      static std::string next_str;
      if (state->next.valid)
        next_str.assign(state->next.data, state->next.size);
      else
        next_str.clear();
      curr_blank = callbacks.is_blank_line(curr_str);
      next_blank = !state->next.valid || callbacks.is_blank_line(next_str);

      bool curr_strong = false;
      curr_candidate =
          callbacks.looks_like_plain_chapter_heading(curr_str, &curr_strong);
      bool next_strong = false;
      bool next_candidate =
          state->next.valid &&
          callbacks.looks_like_plain_chapter_heading(next_str, &next_strong);
      curr_keep_with_next = callbacks.should_accept_heuristic_heading(
          curr_str, state->prev_blank, next_blank, state->prev_candidate,
          next_candidate, curr_strong);

      if (curr_keep_with_next)
        callbacks.add_chapter_if_unique(state->parsedata.book, curr_str, 0);
    }

    const size_t bytes_before = state->text_bytes_fed;
    if (state->curr.size > 0) {
      size_t segment_start = 0;
      size_t pos = 0;
      while (pos < state->curr.size) {
        unsigned char c = (unsigned char)state->curr.data[pos];
        const InlineImageContext image_context =
            (c == TEXT_IMAGE_CONTEXT_DEFAULT)
                ? INLINE_IMAGE_CONTEXT_DEFAULT
                : (c == TEXT_IMAGE_LEADING_PARAGRAPH)
                      ? INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH
                      : (c == TEXT_IMAGE_FIGURE_WITH_CAPTION)
                            ? INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION
                            : INLINE_IMAGE_CONTEXT_DEFAULT;
        const bool context_marker =
            c == TEXT_IMAGE_CONTEXT_DEFAULT ||
            (c == TEXT_IMAGE_LEADING_PARAGRAPH) ||
            c == TEXT_IMAGE_FIGURE_WITH_CAPTION;
        const int heading_level = mobi_heading_markers::HeadingLevelFromMarker(c);
        const bool image_marker =
            (c == TEXT_IMAGE) ||
            (context_marker && pos + 1 < state->curr.size &&
             (unsigned char)state->curr.data[pos + 1] == TEXT_IMAGE);
        if (heading_level > 0) {
          if (pos > segment_start) {
            const size_t len = pos - segment_start;
            xml::book::chardata(&state->parsedata,
                                state->curr.data + segment_start, (int)len);
            state->text_bytes_fed += len;
          }
          callbacks.apply_heading_keep_with_next(&state->parsedata, heading_level);
          state->text_bytes_fed += 1;
          pos++;
          segment_start = pos;
          continue;
        }
        if (!image_marker) {
          pos++;
          continue;
        }

        if (pos > segment_start) {
          const size_t len = pos - segment_start;
          xml::book::chardata(&state->parsedata,
                              state->curr.data + segment_start, (int)len);
          state->text_bytes_fed += len;
        }

        size_t image_pos = pos + (context_marker ? 1 : 0);
        if (image_pos + 2 >= state->curr.size) {
          state->text_bytes_fed += state->curr.size - pos;
          segment_start = state->curr.size;
          break;
        }

        u16 image_id = (u16)(((u8)state->curr.data[image_pos + 1] << 8) |
                             (u8)state->curr.data[image_pos + 2]);
        callbacks.append_inline_image_to_parsedata(&state->parsedata, image_id,
                                                   image_context);
        size_t consumed = context_marker ? 4 : 3;
        state->text_bytes_fed += consumed;
        pos += consumed;
        segment_start = pos;
      }

      if (segment_start < state->curr.size) {
        const size_t len = state->curr.size - segment_start;
        xml::book::chardata(&state->parsedata,
                            state->curr.data + segment_start, (int)len);
        state->text_bytes_fed += len;
      }
    }
    if (state->curr.has_newline) {
      xml::book::chardata(&state->parsedata, "\n", 1);
      state->text_bytes_fed += 1;
    }

    if (text_cursor_per_page) {
      const u16 new_page_count = state->parsedata.book->GetPageCount();
      while (last_page_count < new_page_count) {
        text_cursor_per_page->push_back((u32)bytes_before);
        last_page_count++;
      }
    }

    state->prev_blank = heuristic_headings ? curr_blank : false;
    state->prev_candidate = heuristic_headings ? curr_candidate : false;
    state->curr = state->next;
    state->next = ReadNextLineChunk(text_utf8, &state->cursor);

    const u16 pages_done = state->parsedata.book->GetPageCount() - page_start;
    const bool have_min_pages =
        state->parsedata.book->GetPageCount() >= min_pages_before_stop;
    if (page_budget > 0 && pages_done >= page_budget && have_min_pages)
      break;
    if (budget_ms > 0 && (osGetTime() - t_begin) >= budget_ms && have_min_pages)
      break;
  }

  if (!state->curr.valid) {
    parse_pop(&state->parsedata);
    callbacks.finalize_plain_page(&state->parsedata);
    if (state->detect_heuristic_headings)
      callbacks.set_non_epub_toc_confidence(state->parsedata.book, false);
    else
      state->parsedata.book->ClearTocConfidence();
    state->completed = true;
  }

  if (perf_out) {
    const u64 stream_ms = osGetTime() - t_begin;
    const u32 input_bytes = (u32)(state->text_bytes_fed - bytes_before_stream);
    const u16 pages_generated = state->parsedata.book->GetPageCount() - page_start;
    FillPlainTextStreamPerf(state->parsedata, perf_baseline, stream_ms,
                            input_bytes, pages_generated, perf_out);
  }

  return state->completed;
}

} // namespace plain_text_stream
