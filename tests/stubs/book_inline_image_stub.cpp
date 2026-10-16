/*
 * Stubs for Book inline image methods from book_inline_image.cpp.
 * TXT/FB2 fixture files have no inline images so these paths are not reached
 * at runtime, but the linker needs the symbols defined.
 */
#include "book/book.h"
#include "book/inline_image_layout.h"

#include "book_inline_image_stub_test_api.h"

namespace {

InlineImageMetadata g_stub_meta{};
InlineImageLayoutPlan g_stub_plan{};
bool g_stub_plan_result = false;
InlineImageDrawBounds g_stub_bounds{};
bool g_stub_draw = false;

} // namespace

void ResetBookInlineImageStubState() {
  g_stub_meta = InlineImageMetadata{};
  g_stub_draw = false;
  g_stub_plan = InlineImageLayoutPlan{};
  g_stub_plan_result = false;
}

void ConfigureBookInlineImageStub(const InlineImageMetadata &meta,
                                  const InlineImageLayoutPlan &plan,
                                  bool plan_result) {
  g_stub_meta = meta;
  g_stub_plan = plan;
  g_stub_plan_result = plan_result;
}

void ConfigureBookInlineImageDrawBounds(const InlineImageDrawBounds &bounds) {
  g_stub_bounds = bounds; g_stub_draw = true;
}

u16 Book::RegisterInlineImage(const std::string &) { return 0; }
u32 Book::GetInlineImageCount() const { return 0; }
void Book::SetInlineImageFollowTextLines(u16, u8) {}
u8 Book::GetInlineImageFollowTextLines(u16) const { return 0; }
void Book::SetInlineImageAuthorMaxWidth(u16, int) {}
void Book::ClearInlineImageCache() {}
void Book::ClearInlineImages() {
  inline_image_probe_uf = nullptr;
  inline_images.clear();
  inline_image_cache_bytes = 0;
  fb2_inline_images_bytes = 0;
}
void Book::SetInlineImageProbeZip(void *uf) { inline_image_probe_uf = uf; }
bool Book::StoreFb2InlineImage(const std::string &, const std::string &) {
  return false;
}
bool Book::LoadInlineImageSource(u16, image_memory::Bytes *,
                                 std::string *) {
  return false;
}
bool Book::EnsureInlineImageMetadata(u16, InlineImageMetadata *out) {
  if (out)
    *out = g_stub_meta;
  return g_stub_meta.ok;
}
bool Book::GetInlineImageMetadata(u16, InlineImageMetadata *out) {
  if (out)
    *out = g_stub_meta;
  return g_stub_meta.ok;
}
bool Book::PlanInlineImageLayout(Text *, u16, int, int, int, bool,
                                 InlineImageContext,
                                 InlineImageLayoutPlan *plan,
                                 int) {
  if (plan)
    *plan = g_stub_plan;
  return g_stub_plan_result;
}
bool Book::DrawInlineImage(Text *, u16, const InlineImageLayoutPlan *, int,
                           u8, InlineImageDrawBounds *bounds) {
  if (bounds) *bounds = g_stub_bounds;
  return g_stub_draw;
}

const std::string *Book::GetInlineImagePath(u16) const { return nullptr; }
