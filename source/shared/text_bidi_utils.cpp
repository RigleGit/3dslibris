#include "shared/text_bidi_utils.h"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <algorithm>
#include <cstring>

extern "C" {
#include "mupdf/fitz/bidi.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/document.h"
#include "mupdf/fitz/system.h"
}

namespace text_bidi_utils {

namespace {

// Cached fz_context — created on first use, never freed (3DS lifecycle).
static fz_context *g_bidi_ctx = NULL;
static bool g_bidi_ctx_init_attempted = false;

struct BidiCollectState {
  const uint32_t *base;
  size_t count;
  std::vector<BidiRun> *runs;
  size_t fallback_cursor;
};

static fz_context *GetBidiContext() {
  if (!g_bidi_ctx_init_attempted) {
    g_bidi_ctx_init_attempted = true;
    g_bidi_ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (g_bidi_ctx) {
      fz_register_document_handlers(g_bidi_ctx);
    }
  }
  return g_bidi_ctx;
}

// Callback para fz_bidi_fragment_text: acumula BidiRun en un vector.
static void BidiFragmentCallback(const uint32_t *fragment,
                                  size_t fragmentLen,
                                  int bidiLevel,
                                  int /*script*/,
                                  void *arg) {
  BidiCollectState *state = static_cast<BidiCollectState *>(arg);
  if (!state || !state->runs || fragmentLen == 0)
    return;

  size_t start = state->fallback_cursor;
  if (state->base && fragment >= state->base &&
      fragment < (state->base + state->count)) {
    start = (size_t)(fragment - state->base);
  }

  if (start + fragmentLen > state->count) {
    if (start >= state->count)
      return;
    fragmentLen = state->count - start;
  }

  BidiRun run;
  run.bidi_level = bidiLevel;
  run.length = fragmentLen;
  run.start = start;
  if (state->runs->empty()) {
    state->runs->push_back(run);
  } else {
    BidiRun &prev = state->runs->back();
    // Merge contiguous runs at same embedding level.
    if (prev.bidi_level == run.bidi_level &&
        (prev.start + prev.length) == run.start) {
      prev.length += run.length;
    } else {
      state->runs->push_back(run);
    }
  }
  state->fallback_cursor = run.start + run.length;
}

} // namespace

bool ContainsRTL(const uint32_t *codepoints, size_t count) {
  for (size_t i = 0; i < count; i++) {
    uint32_t cp = codepoints[i];
    // Hebrew: U+0590-U+05FF
    if (cp >= 0x0590 && cp <= 0x05FF) return true;
    // Arabic: U+0600-U+06FF
    if (cp >= 0x0600 && cp <= 0x06FF) return true;
    // Arabic Extended-A: U+08A0-U+08FF
    if (cp >= 0x08A0 && cp <= 0x08FF) return true;
    // Arabic Presentation Forms-A: U+FB50-U+FDFF
    if (cp >= 0xFB50 && cp <= 0xFDFF) return true;
    // Arabic Presentation Forms-B: U+FE70-U+FEFF
    if (cp >= 0xFE70 && cp <= 0xFEFF) return true;
  }
  return false;
}

bool AnalyzeBidiRuns(const uint32_t *codepoints, size_t count,
                     std::vector<BidiRun> *runs) {
  if (!runs || !codepoints || count == 0) return false;

  runs->clear();

  fz_context *ctx = GetBidiContext();
  if (!ctx) {
    // Sin MuPDF: fallback a un solo run LTR.
    BidiRun run;
    run.start = 0;
    run.length = count;
    run.bidi_level = 0;
    runs->push_back(run);
    return false;
  }

  BidiCollectState collect_state;
  collect_state.base = codepoints;
  collect_state.count = count;
  collect_state.runs = runs;
  collect_state.fallback_cursor = 0;

  fz_bidi_direction baseDir = FZ_BIDI_NEUTRAL;
  fz_bidi_fragment_fn *callback = BidiFragmentCallback;
  fz_bidi_fragment_text(ctx, codepoints, count, &baseDir, callback,
                        &collect_state, 0);

  if (runs->empty()) {
    // Sin fragmentos: todo es LTR.
    BidiRun run;
    run.start = 0;
    run.length = count;
    run.bidi_level = 0;
    runs->push_back(run);
  }

  return true;
}

void ReorderLineForDisplay(std::vector<uint32_t> &codepoints,
                            size_t line_start, size_t line_end,
                            const std::vector<BidiRun> &runs) {
  if (line_start >= line_end || line_end > codepoints.size()) return;
  if (runs.empty()) return;

  const size_t n = line_end - line_start;
  std::vector<int> levels(n, 0);
  int max_level = 0;
  int min_odd_level = 126;

  // Materialize resolved bidi level per codepoint in the current line.
  for (size_t r = 0; r < runs.size(); r++) {
    const BidiRun &run = runs[r];
    size_t r_start = run.start;
    size_t r_end = run.start + run.length;
    size_t i_start = (r_start < line_start) ? line_start : r_start;
    size_t i_end = (r_end > line_end) ? line_end : r_end;
    if (i_start >= i_end)
      continue;

    const int level = run.bidi_level;
    if (level > max_level)
      max_level = level;
    if ((level & 1) && level < min_odd_level)
      min_odd_level = level;

    for (size_t i = i_start; i < i_end; i++)
      levels[i - line_start] = level;
  }

  if (min_odd_level == 126)
    return;

  // UAX#9 L2: reverse contiguous sequences whose level >= current level,
  // iterating from max level down to min odd level.
  for (int level = max_level; level >= min_odd_level; level--) {
    size_t i = 0;
    while (i < n) {
      while (i < n && levels[i] < level)
        i++;
      const size_t seq_start = i;
      while (i < n && levels[i] >= level)
        i++;
      const size_t seq_end = i;
      if (seq_end > seq_start + 1) {
        std::reverse(codepoints.begin() + line_start + seq_start,
                     codepoints.begin() + line_start + seq_end);
      }
    }
  }
}

} // namespace text_bidi_utils
