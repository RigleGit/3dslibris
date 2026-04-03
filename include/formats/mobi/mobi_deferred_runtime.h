/*
    3dslibris - mobi_deferred_runtime.h

    Deferred MOBI pagination/finalization runtime state machine.
*/

#pragma once

#include <3ds/types.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/common/plain_text_stream.h"
#include "formats/mobi/mobi_structured_toc_parser.h"
#include "formats/mobi/mobi_toc_finalize.h"

class Book;

namespace mobi_deferred_runtime {

struct State {
  plain_text_stream::State stream;
  std::string source_path;
  std::string markup_utf8;
  std::string text_utf8;
  std::vector<mobi_toc_finalize::MobiHeadingHint> heading_hints;
  std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> structured_toc;
  std::vector<std::pair<u32, u32>> html_to_text_map;
  std::vector<u32> text_cursor_per_page;
  bool have_structured_toc;
  bool structured_from_filepos;
  bool toc_metadata_ready;
  bool structured_toc_loaded;
  bool toc_applied;
  bool cache_saved;
  bool used_utf8_guess;
  bool used_legacy_guess;
  bool line_wrap_fix_applied;
  bool finalized;
  u32 text_len_for_pos;
  u64 t_parse_begin;
  u64 t_after_read;
  u64 t_after_decompress;
  u64 t_after_decode;
  u64 t_after_markup_scan;
  u64 t_after_cleanup;
  u64 t_after_initial_pages;
  u64 t_after_markup;
  u64 t_after_pages;
  u64 t_after_toc;

  State();
};

struct FinalizeCallbacks {
  void (*build_toc_metadata)(Book *book, State *state);
  void (*load_structured_toc)(Book *book, State *state);
  void (*apply_toc)(Book *book, State *state);
  void (*save_cache)(Book *book, State *state);
};

bool Has(const Book *book);
State *Find(const Book *book);
void Put(const Book *book, State &&state);
void Erase(const Book *book);

bool Finalize(Book *book, State *state, const FinalizeCallbacks &callbacks);

bool Continue(Book *book, State *state, u32 budget_ms, u16 page_budget,
              const plain_text_stream::ContinueCallbacks &stream_callbacks,
              const FinalizeCallbacks &finalize_callbacks);

} // namespace mobi_deferred_runtime
