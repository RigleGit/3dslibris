#include "formats/common/fixed_layout_preview_constants.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/app_flow_utils.h"
#include "shared/touch_map_utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

// Platform handles and the Book format/state adapter are supplied by the test.
// Viewport state and geometry helpers below are production definitions;
// no MuPDF document rendering is involved.
typedef unsigned char u8;
struct fz_context;
struct fz_document;
struct fz_outline;
struct fz_display_list;
struct LightEvent {};
typedef void *Thread;
#include "book/book.h"
#include "formats/mupdf/mupdf_state.h"
static const int kPdfPreviewPadding=fixed_layout_preview::kPadding;
static const int kPdfPreviewScreenWidth=fixed_layout_screen::kBottomScreenWidth;
static const int kPdfPreviewScreenHeight=fixed_layout_screen::kBottomScreenHeight;
#include "mupdf_viewport_touch_under_test.inc"

static void CheckCenter(const Book::MuPdfState &state, float x, float y) {
  assert(std::abs(state.viewport.center_x - x) < 0.0001f);
  assert(std::abs(state.viewport.center_y - y) < 0.0001f);
}

int main() {
  struct TouchCase {
    unsigned char orientation;
    int raw_center_x, raw_center_y, raw_quarter_x, raw_quarter_y;
  };
  // A square preview has a 232px side with the current 4px panel padding.
  // Portrait: (4,44)..(236,276); landscape: (44,4)..(276,236).
  // Raw inputs below target its center, then its 25%/25% point.
  const TouchCase cases[] = {
      {orientation_utils::ORIENT_TURNED_LEFT, 159, 120, 217, 62},
      {orientation_utils::ORIENT_TURNED_RIGHT, 160, 119, 102, 177},
      {orientation_utils::ORIENT_LANDSCAPE, 160, 120, 102, 62},
  };
  for (const TouchCase &test : cases) {
    Book::MuPdfState state;
    const auto top=fixed_layout_screen::TargetDims(test.orientation, true);
    const auto bottom=fixed_layout_screen::TargetDims(test.orientation, false);
    state.target_top_width=top.width; state.target_top_height=top.height;
    state.target_bottom_width=bottom.width; state.target_bottom_height=bottom.height;
    state.page_width=1000; state.page_height=1000;
    state.viewport.zoom_index=6;
    state.current_preview.page=0;
    state.current_preview.bitmap_width=232;
    state.current_preview.bitmap_height=232;
    state.current_preview.pixels.push_back(0);
    Book book; book.mupdf_state=&state;
    const auto center=touch_map_utils::MapRawTouch(
        test.orientation, test.raw_center_x, test.raw_center_y);
    assert(!book.MoveMuPdfViewportToPreview(center.x, center.y));
    CheckCenter(state, 0.5f, 0.5f);
    const auto quarter=touch_map_utils::MapRawTouch(
        test.orientation, test.raw_quarter_x, test.raw_quarter_y);
    assert(book.MoveMuPdfViewportToPreview(quarter.x, quarter.y));
    CheckCenter(state, 0.25f, 0.25f);
  }
  puts("PASS: MuPDF overview touch follows its logical panel in every orientation");
}
