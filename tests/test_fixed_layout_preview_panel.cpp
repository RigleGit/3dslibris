#include "formats/common/fixed_layout_preview_panel.h"
#include "formats/common/fixed_layout_preview_constants.h"
#include "book/book.h"
#include "ui/text.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace panel = fixed_layout_preview_panel;

struct Fixture {
  Text text;
  Book book;
  panel::State state;
  std::vector<u16> pixels;
  pdf_view_utils::PreviewLayout layout;
  int page, zoom;
  bool wide;

  Fixture(int width, int height, int orientation = 0)
      : text(width, height, orientation), book(&text), pixels(7 * 11),
        page(2), zoom(3), wide(false) {
    layout.x = 4; layout.y = 6; layout.width = 23; layout.height = 31;
    for (size_t i = 0; i < pixels.size(); ++i)
      pixels[i] = (u16)(i * 1739 + 27);
  }
  void Draw(const panel::Rect &viewport) {
    panel::Draw(&book, &text, &state, pixels.data(), pixels.size(),
                7, 11, page, zoom, wide, layout, viewport);
  }
  void ExpectFullReference(const panel::Rect &viewport) {
    Text expected(text.width, text.height, text.orientation);
    expected.color_mode = text.color_mode;
    Book expected_book(&expected);
    expected_book.background_available = book.background_available;
    if (!expected_book.DrawBottomGradientBackground()) expected.ClearScreen();
    fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(
        expected.screenright, expected.stride, expected.width, expected.height,
        layout.x, layout.y, layout.width, layout.height,
        pixels.data(), pixels.size(), 7, 11, 0, 0, 7, 11, true);
    expected.DrawRect(layout.x, layout.y, layout.x + layout.width,
                      layout.y + layout.height, fixed_layout_preview::kFrame);
    expected.DrawRect(viewport.x, viewport.y, viewport.x + viewport.width,
                      viewport.y + viewport.height,
                      fixed_layout_preview::kViewportAccent);
    assert(text.bottom == expected.bottom &&
           "incremental preview must equal the original full redraw, including padding");
  }
};

void TestPanningMatchesFullRedrawWithoutBackgroundWork() {
  // Both portrait lateralities, landscape, and wide-top selection keep the
  // bottom panel's logical pixels and scale exact.
  for (int orientation = 0; orientation < 3; ++orientation) {
    Fixture f(orientation == 2 ? 320 : 240,
              orientation == 2 ? 240 : 320, orientation);
    f.wide = orientation != 0;
    panel::Rect rect = {4, 6, 13, 15};
    f.Draw(rect);
    f.ExpectFullReference(rect);
    const int draws = f.book.background_draws;
    for (int step = 0; step < 35; ++step) {
      f.text.Present();
      rect = {4 + step % 11, 6 + step % 17, 13, 15};
      f.Draw(rect);
      f.ExpectFullReference(rect);
      assert(f.book.background_draws == draws &&
             "viewport pan must not redraw the gradient");
    }
    // Degenerate borders and old/new frame overlaps use the same draw order.
    const panel::Rect corners[] = {{4,6,1,1},{26,36,1,1},{4,6,23,31},
                                    {4,6,1,31},{4,6,23,1},{7,9,2,2}};
    for (const panel::Rect &corner : corners) {
      f.text.Present();
      f.Draw(corner);
      f.ExpectFullReference(corner);
      assert(f.book.background_draws == draws);
    }
    f.text.Present();
    const unsigned writes = f.text.writes;
    f.text.screenleft_dirty = true;
    f.Draw(corners[5]);
    assert(f.text.writes == writes && !f.text.screenright_dirty &&
           "unchanged bottom pixels must be skipped even if top is dirty");
  }
}

void TestNativeSizePreviewRestoresClippedStrips() {
  Fixture f(32, 40);
  f.layout = {27, 33, 7, 11};
  panel::Rect rect = {27, 33, 4, 5};
  f.Draw(rect);
  const int backgrounds = f.book.background_draws;
  f.text.Present();
  rect = {29, 36, 4, 5};
  f.Draw(rect);
  f.ExpectFullReference(rect);
  assert(f.book.background_draws == backgrounds);
}

void TestExternalInvalidationAndContentChanges() {
  Fixture f(240, 320);
  const panel::Rect rect = {5, 8, 14, 17};
  f.Draw(rect);
  for (int change = 0; change < 11; ++change) {
    f.text.Present();
    const int before = f.book.background_draws;
    switch (change) {
    case 0: // Unpresented menu or HOME resume marks bottom dirty.
      f.text.FillRect(0, 0, 30, 40, 0xdead); break;
    case 1: // A menu can have been presented before returning to the reader.
      f.text.FillRect(0, 0, 30, 40, 0xbeef); f.text.Present(); break;
    case 2: ++f.page; break;
    case 3: ++f.zoom; break;
    case 4: ++f.text.color_mode; break;
    case 5: ++f.text.orientation; break;
    case 6: f.wide = !f.wide; break;
    case 7: ++f.layout.x; break;
    case 8: { // Preview replacement even with unchanged dimensions.
      std::vector<u16> replacement(f.pixels.size(), 0xf00d);
      f.pixels.swap(replacement); break;
    }
    case 9: // Explicit invalidation covers allocation reuse after rebuilding.
      f.state.valid = false; f.pixels[0] ^= 0xffff; break;
    case 10: // A second draw before presentation must not assume a clean panel.
      { panel::Rect next = rect; ++next.x; f.Draw(next); } break;
    }
    f.Draw(rect);
    f.ExpectFullReference(rect);
    assert(f.book.background_draws > before &&
           "changed content or external drawing requires a full preview redraw");
  }
}

void TestClippingAndOutlyingRoundedViewport() {
  Fixture f(32, 40);
  f.layout.x = 19; f.layout.y = 22; // Preview clips against both panel edges.
  panel::Rect rect = {20, 24, 11, 15};
  f.Draw(rect);
  f.text.Present();
  rect = {21, 25, 10, 14};
  f.Draw(rect);
  f.ExpectFullReference(rect);
  // Separately rounded viewport origin and size can exceed the frame by one.
  rect = {f.layout.x + 15, f.layout.y + 20, 9, 12};
  f.text.Present(); f.Draw(rect); f.ExpectFullReference(rect);
  f.text.Present(); rect = {20, 24, 11, 15};
  f.Draw(rect); f.ExpectFullReference(rect);
}

void TestInteriorPanDamagesOnlyTheViewport() {
  Fixture f(240, 320);
  f.layout = {20, 30, 150, 240};
  f.Draw({50, 80, 30, 50});
  f.text.Present();
  f.Draw({55, 85, 30, 50});
  f.ExpectFullReference({55, 85, 30, 50});
  assert(f.text.dirty_x0 == 50 && f.text.dirty_y0 == 80 &&
         f.text.dirty_x1 == 85 && f.text.dirty_y1 == 135 &&
         "interior pan must not invalidate the whole preview frame");
}

int main() {
  TestPanningMatchesFullRedrawWithoutBackgroundWork();
  TestNativeSizePreviewRestoresClippedStrips();
  TestExternalInvalidationAndContentChanges();
  TestClippingAndOutlyingRoundedViewport();
  TestInteriorPanDamagesOnlyTheViewport();
  puts("Fixed-layout preview panel passed");
}
