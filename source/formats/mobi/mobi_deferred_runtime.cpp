/*
    3dslibris - mobi_deferred_runtime.cpp
*/

#include "formats/mobi/mobi_deferred_runtime.h"

#include <unordered_map>

#include <3ds.h>

#include "formats/common/plain_text_perf_utils.h"
#include "formats/mobi/mobi_deferred_finalize_utils.h"

namespace mobi_deferred_runtime {

State::State()
    : have_structured_toc(false), structured_from_filepos(false),
      toc_metadata_ready(false), structured_toc_loaded(false),
      toc_applied(false), cache_saved(false), used_utf8_guess(false),
      used_legacy_guess(false), line_wrap_fix_applied(false), finalized(false),
      text_len_for_pos(0), t_parse_begin(0), t_after_read(0),
      t_after_decompress(0), t_after_decode(0), t_after_markup_scan(0),
      t_after_cleanup(0), t_after_initial_pages(0), t_after_markup(0),
      t_after_pages(0), t_after_toc(0) {}

static std::unordered_map<const Book *, State> g_states;

bool Has(const Book *book) { return g_states.find(book) != g_states.end(); }

State *Find(const Book *book) {
  auto it = g_states.find(book);
  if (it == g_states.end())
    return nullptr;
  return &it->second;
}

void Put(const Book *book, State &&state) { g_states[book] = std::move(state); }

void Erase(const Book *book) { g_states.erase(book); }

bool Finalize(Book *book, State *state, const FinalizeCallbacks &callbacks) {
  if (!book || !state)
    return true;
  if (state->finalized)
    return true;
  if (!state->stream.completed)
    return false;

  switch (mobi_deferred_finalize_utils::NextFinalizeStage(
      state->stream.completed, state->toc_metadata_ready,
      state->structured_toc_loaded, state->toc_applied, state->cache_saved,
      state->finalized)) {
  case mobi_deferred_finalize_utils::FinalizeStage::BuildMetadata:
    if (callbacks.build_toc_metadata)
      callbacks.build_toc_metadata(book, state);
    state->toc_metadata_ready = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::LoadStructuredToc:
    if (callbacks.load_structured_toc)
      callbacks.load_structured_toc(book, state);
    state->structured_toc_loaded = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::ApplyToc:
    if (callbacks.apply_toc)
      callbacks.apply_toc(book, state);
    state->toc_applied = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::SaveCache:
    if (callbacks.save_cache)
      callbacks.save_cache(book, state);
    state->cache_saved = true;
    return false;
  case mobi_deferred_finalize_utils::FinalizeStage::Done:
    state->finalized = true;
    return true;
  case mobi_deferred_finalize_utils::FinalizeStage::ContinuePaging:
  default:
    return false;
  }
}

bool Continue(Book *book, State *state, u32 budget_ms, u16 page_budget,
              const plain_text_stream::ContinueCallbacks &stream_callbacks,
              const FinalizeCallbacks &finalize_callbacks) {
  if (!book || !state)
    return true;

  const u16 pages_before = book->GetPageCount();
  plain_text_perf_utils::PlainTextStreamPerf perf;
  const bool done = plain_text_stream::ContinueState(
      &state->stream, state->text_utf8, budget_ms, page_budget, 0,
      &state->text_cursor_per_page, &perf, stream_callbacks);
  if (book->GetPageCount() > pages_before)
    state->t_after_pages = osGetTime();

  if (!done)
    return false;
  return Finalize(book, state, finalize_callbacks);
}

} // namespace mobi_deferred_runtime
