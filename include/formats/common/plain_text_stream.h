/*
    3dslibris - plain_text_stream.h

    Incremental plain text pagination stream helpers.
*/

#pragma once

#include <string>
#include <vector>

#include <3ds/types.h>

#include "book/book.h"
#include "formats/common/plain_text_perf_utils.h"
#include "parse.h"

namespace plain_text_stream {

// A view into a line of text inside the source text_utf8 buffer.
// The data/size pointers are valid as long as the source string is alive and
// unmodified. No heap allocation — assign-by-value is a cheap struct copy.
struct PlainLineChunk {
  const char *data;
  size_t size;
  bool has_newline;
  bool valid;
};

struct State {
  parsedata_t parsedata;
  size_t cursor;
  PlainLineChunk curr;
  PlainLineChunk next;
  bool prev_blank;
  bool prev_candidate;
  bool detect_heuristic_headings;
  bool initialized;
  bool completed;
  size_t text_bytes_fed;
};

struct ContinueCallbacks {
  bool (*is_blank_line)(const std::string &line);
  bool (*looks_like_plain_chapter_heading)(const std::string &line, bool *is_strong);
  bool (*should_accept_heuristic_heading)(const std::string &line, bool prev_blank,
                                          bool next_blank, bool prev_candidate,
                                          bool next_candidate, bool is_strong);
  void (*add_chapter_if_unique)(Book *book, const std::string &title, u8 level);
  bool (*apply_heading_keep_with_next)(parsedata_t *p, int heading_level);
  void (*append_inline_image_to_parsedata)(parsedata_t *p, u16 image_id,
                                           InlineImageContext image_context);
  void (*finalize_plain_page)(parsedata_t *p);
  void (*set_non_epub_toc_confidence)(Book *book, bool strong);
};

void InitState(State *state, const parsedata_t &base_parsedata,
               const std::string &text_utf8, bool detect_heuristic_headings);

bool ContinueState(State *state, const std::string &text_utf8, u32 budget_ms,
                   u16 page_budget, u16 min_pages_before_stop,
                   std::vector<u32> *text_cursor_per_page,
                   plain_text_perf_utils::PlainTextStreamPerf *perf_out,
                   const ContinueCallbacks &callbacks);

} // namespace plain_text_stream

