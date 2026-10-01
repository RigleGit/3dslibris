#include <cassert>
#include <cstdio>
#include <cstddef>
#include <vector>
#include <string>
struct fz_context {};
struct fz_document {};
struct fz_display_list { int page; };
struct IStatusReporter {};
struct RenderedMuPdfBitmap { int width = 10, height = 10; };
#define DBG_LOGF_CAT(...) ((void)0)
namespace app_flow_utils { enum class MuPdfDocumentKind { Pdf }; }
namespace mupdf_render_policy_utils {
  static bool skip = false;
  bool ShouldSkipPdfPageRender(app_flow_utils::MuPdfDocumentKind, int, size_t) { return skip; }
}
struct Cache { int page = -1; };
struct Book {
  struct MuPdfState {
    fz_context context;
    fz_document document;
    fz_context *ctx = &context;
    fz_document *doc = &document;
    IStatusReporter *reporter = nullptr;
    Cache current_preview, current_interactive_tile, current_final_zoom;
    Cache prev_slot, next_slot;
    bool final_cache_pending = true;
    float page_width = 600, page_height = 900;
    int target_bottom_width = 240, target_bottom_height = 320;
    app_flow_utils::MuPdfDocumentKind document_kind = app_flow_utils::MuPdfDocumentKind::Pdf;
    int page_too_complex_for_device = -1, cached_display_list_page = -1;
    fz_display_list *cached_display_list = nullptr;
  };
};
static int renders = 0, builds = 0, drops = 0, cancels = 0;
static bool fail = false;
static std::vector<std::string> events;
bool PromoteMuPdfAdjacentSlotIfMatching(Book::MuPdfState *, int) { return false; }
void CancelMuPdfIncrementalRenderState(Book::MuPdfState *) { ++cancels; events.push_back("cancel"); }
void fz_drop_display_list(fz_context *, fz_display_list *p) { assert(!events.empty() && events.back() == "cancel" && "each old list must be cancelled before disposal"); events.push_back("drop"); ++drops; delete p; }
bool BitmapCacheValid(const Cache &c, int p) { return c.page == p; }
float ComputeMuPdfPreviewScale(float, float, int, int) { return 0.2f; }
bool EstimateMuPdfPageRenderComplexity(fz_context *, fz_document *, int, int *, size_t *) { return true; }
bool RenderMuPdfBitmap(fz_context *, fz_document *, int p, float, RenderedMuPdfBitmap *, float *, float *, const void *, fz_display_list *reuse, fz_display_list **out, IStatusReporter *, const char * = nullptr) {
  ++renders; events.push_back("render");
  if (fail) return false;
  if (reuse) assert(reuse->page == p);
  else if (out) { *out = new fz_display_list{p}; ++builds; }
  return true;
}
void ComputeBitmapContentBoundsNormalized(const RenderedMuPdfBitmap &, float *, float *, float *, float *) {}
void StoreBitmapCache(Cache *c, int p, int, float, float, float, float, RenderedMuPdfBitmap *) { c->page = p; }
void ResetBitmapCache(Cache *c) { c->page = -1; }
void ResetAdjacentSlot(Cache *c, fz_context *) { c->page = -1; }
void ResetMuPdfRenderFailureState(Book::MuPdfState *) {}
#include "mupdf_preview_under_test.inc"
int main() {
  Book::MuPdfState s;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 0));
  assert(builds == 1 && s.cached_display_list && "preview must retain interpretation for the main view");
  fz_display_list *list = nullptr;
  assert(EnsureMuPdfDisplayListForPage(&s, 0, &list));
  assert(list == s.cached_display_list && list->page == 0);
  s.current_interactive_tile.page = 0;
  s.current_final_zoom.page = 0;
  ResetMuPdfDeferredCachesForSynchronousRender(&s);
  assert(s.cached_display_list == list && drops == 0 && "zoom must preserve page interpretation");
  assert(s.cached_display_list_page == 0 && s.current_preview.page == 0);
  assert(s.current_interactive_tile.page == -1 && s.current_final_zoom.page == -1);
  assert(cancels > 0 && !s.final_cache_pending);

  assert(EnsureCurrentMuPdfPreviewCache(&s, 0) && renders == 1);
  // Rebuilding just the bitmap (e.g. geometry) can reuse the same list.
  s.current_preview.page = -1;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 0) && builds == 1);
  events.clear();
  assert(EnsureCurrentMuPdfPreviewCache(&s, 1) && builds == 2 && drops == 1);
  assert((events == std::vector<std::string>{"cancel", "drop", "render"}));
  fail = true;
  assert(!EnsureCurrentMuPdfPreviewCache(&s, 2));
  assert(!s.cached_display_list && drops == 2);
  assert(s.current_preview.page == 1 && s.cached_display_list_page == -1);
  fail = false;
  assert(EnsureCurrentMuPdfPreviewCache(&s, 2));
  assert(builds == 3 && s.cached_display_list->page == 2 && s.current_preview.page == 2);
  const int before_skip=renders;
  mupdf_render_policy_utils::skip = true;
  assert(!EnsureCurrentMuPdfPreviewCache(&s, 3));
  assert(s.page_too_complex_for_device == 3 && builds == 3 && renders == before_skip);
  assert(s.current_preview.page == 2 && s.cached_display_list->page == 2);
  assert(!EnsureMuPdfDisplayListForPage(&s, 2, nullptr));
  assert(!EnsureMuPdfDisplayListForPage(nullptr, 2, &list));
  assert(!EnsureCurrentMuPdfPreviewCache(nullptr, 0));
  events.push_back("cancel"); fz_drop_display_list(s.ctx, s.cached_display_list);
  puts("MuPDF preview list reuse passed");
}
