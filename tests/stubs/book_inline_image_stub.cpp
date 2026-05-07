/*
 * Stubs for Book inline image methods from book_inline_image.cpp.
 * TXT/FB2 fixture files have no inline images so these paths are not reached
 * at runtime, but the linker needs the symbols defined.
 */
#include "book/book.h"
#include "book/inline_image_layout.h"

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
bool Book::LoadInlineImageSource(u16, std::vector<u8> *,
                                 std::string *) {
  return false;
}
bool Book::EnsureInlineImageMetadata(u16, InlineImageMetadata *out) {
  if (out) { out->ok = false; out->width = 0; out->height = 0; }
  return false;
}
bool Book::GetInlineImageMetadata(u16, InlineImageMetadata *out) {
  if (out) { out->ok = false; out->width = 0; out->height = 0; }
  return false;
}
bool Book::PlanInlineImageLayout(Text *, u16, int, int, int, bool,
                                 InlineImageContext,
                                 InlineImageLayoutPlan *plan,
                                 int) {
  if (plan) {
    plan->mode = INLINE_IMAGE_LAYOUT_INLINE;
    plan->draw_width = 0; plan->draw_height = 0;
    plan->line_break_before = false; plan->advance_before = false;
    plan->consume_rest_of_screen = false;
    plan->vertical_space_after_draw = 0;
    plan->next_text_screen = 0; plan->page_breaks = 0;
  }
  return false;
}
bool Book::DrawInlineImage(Text *, u16, const InlineImageLayoutPlan *, int,
                           u8) {
  return false;
}

const std::string *Book::GetInlineImagePath(u16) const { return nullptr; }
