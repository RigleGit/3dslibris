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
  std::vector<BidiRun> *runs = static_cast<std::vector<BidiRun> *>(arg);
  // Calcular offset absoluto desde el puntero del fragmento.
  // El caller pasa codepoints como base; el fragment pointer es dentro de ese array.
  // Pero no tenemos la base aquí — usamos un truco: el callback recibe
  // fragmentos en orden secuencial, así que podemos acumular start/length.
  BidiRun run;
  run.bidi_level = bidiLevel;
  run.length = fragmentLen;
  if (runs->empty()) {
    run.start = 0;
  } else {
    const BidiRun &prev = runs->back();
    run.start = prev.start + prev.length;
  }
  runs->push_back(run);
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

  fz_bidi_direction baseDir = FZ_BIDI_NEUTRAL;
  fz_bidi_fragment_fn *callback = BidiFragmentCallback;
  fz_bidi_fragment_text(ctx, codepoints, count, &baseDir,
                        callback, runs, 0);

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

  // Para cada run RTL en el rango de la línea, invertir los codepoints.
  // UAX#9 L2: los niveles impares se invierten.
  for (size_t r = 0; r < runs.size(); r++) {
    const BidiRun &run = runs[r];
    if (run.bidi_level & 1) {
      // RTL run: invertir.
      size_t r_start = run.start;
      size_t r_end = run.start + run.length;
      // Intersección con el rango de la línea.
      size_t i_start = (r_start < line_start) ? line_start : r_start;
      size_t i_end = (r_end > line_end) ? line_end : r_end;
      if (i_start < i_end && i_end - i_start > 1) {
        std::reverse(codepoints.begin() + i_start,
                     codepoints.begin() + i_end);
      }
    }
  }
}

} // namespace text_bidi_utils
