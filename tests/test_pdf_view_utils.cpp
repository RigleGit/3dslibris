#include "formats/common/pdf_view_utils.h"
#include "formats/common/fixed_layout_viewport_utils.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectNear(const char *label, float actual, float expected,
                float epsilon = 0.0001f) {
  if (fabs(actual - expected) > epsilon) {
    Fail(std::string(label) + ": expected near equality");
  }
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected equality");
}

void ExpectEqU32(const char *label, unsigned int actual, unsigned int expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected equality");
}

void TestZoomPresets() {
  ExpectEq("zoom index below range clamps", pdf_view_utils::ClampZoomIndex(-4),
           0);
  ExpectEq("zoom index above range clamps", pdf_view_utils::ClampZoomIndex(99),
           6);
  ExpectEq("default zoom index", pdf_view_utils::DefaultZoomIndex(), 2);
  ExpectNear("zoom preset 0.5", pdf_view_utils::ZoomForIndex(0), 0.5f);
  ExpectNear("zoom preset 1.0", pdf_view_utils::ZoomForIndex(2), 1.0f);
  ExpectNear("zoom preset 3.0", pdf_view_utils::ZoomForIndex(5), 3.0f);
  ExpectNear("zoom preset 4.0", pdf_view_utils::ZoomForIndex(6), 4.0f);
}

void TestDevicePolicies() {
  const pdf_view_utils::DevicePolicy old_policy =
      pdf_view_utils::GetDevicePolicy(false);
  const pdf_view_utils::DevicePolicy new_policy =
      pdf_view_utils::GetDevicePolicy(true);

  ExpectEq("old 3ds default zoom", old_policy.default_zoom_index, 2);
  ExpectEq("new 3ds default zoom", new_policy.default_zoom_index, 2);
  ExpectEq("old 3ds max zoom", old_policy.max_zoom_index, 6);
  ExpectEq("new 3ds max zoom", new_policy.max_zoom_index, 6);
  ExpectTrue("old 3ds keeps preview cache", old_policy.keep_preview_cache);
  ExpectTrue("new 3ds keeps preview cache", new_policy.keep_preview_cache);
  ExpectTrue("new 3ds keeps tile cache", new_policy.keep_tile_cache);
  ExpectTrue("old 3ds drops tile cache", !old_policy.keep_tile_cache);
  ExpectEqU32("old 3ds MuPDF store bytes", old_policy.mupdf_store_bytes,
              4u * 1024u * 1024u);
  ExpectEqU32("new 3ds MuPDF store bytes", new_policy.mupdf_store_bytes,
              20u * 1024u * 1024u);

  ExpectEq("old 3ds clamps to top zoom tier",
           pdf_view_utils::ClampZoomIndexForDevice(99, false), 6);
  ExpectEq("new 3ds clamps to top zoom tier",
           pdf_view_utils::ClampZoomIndexForDevice(99, true), 6);

  ExpectEq("interactive zoom cannot go below fit",
           pdf_view_utils::ClampInteractiveZoomIndexForDevice(-99, false), 2);
  ExpectEq("interactive zoom keeps levels above fit",
           pdf_view_utils::ClampInteractiveZoomIndexForDevice(3, false), 3);
  ExpectEq("interactive zoom keeps device maximum",
           pdf_view_utils::ClampInteractiveZoomIndexForDevice(99, true), 6);
}

void TestPreviewFit() {
  pdf_view_utils::PreviewLayout fit =
      pdf_view_utils::ComputePreviewLayout(1000.0f, 2000.0f, 240, 320);
  ExpectEq("preview x centers portrait page", fit.x, 40);
  ExpectEq("preview y portrait starts at top", fit.y, 0);
  ExpectEq("preview width portrait", fit.width, 160);
  ExpectEq("preview height portrait", fit.height, 320);
}

void TestPreviewFitInsideInsetBounds() {
  pdf_view_utils::PreviewLayout fit = pdf_view_utils::ComputePreviewLayoutInBounds(
      1000.0f, 2000.0f, 8, 8, 224, 304);
  ExpectEq("inset preview x centers portrait page", fit.x, 44);
  ExpectEq("inset preview y respects top inset", fit.y, 8);
  ExpectEq("inset preview width portrait", fit.width, 152);
  ExpectEq("inset preview height portrait", fit.height, 304);
}

void TestViewportClampAndTouchRecenter() {
  pdf_view_utils::NormalizedRect rect =
      pdf_view_utils::ComputeViewportRect(1000.0f, 600.0f, 2.0f, 400.0f,
                                          240.0f, 0.5f, 0.5f);
  ExpectNear("viewport width at 2x", rect.width, 0.5f);
  ExpectNear("viewport height at 2x", rect.height, 0.5f);
  ExpectNear("viewport left centered", rect.left, 0.25f);
  ExpectNear("viewport top centered", rect.top, 0.25f);

  pdf_view_utils::NormalizedPoint center =
      pdf_view_utils::RecenterViewportFromPreview(pdf_view_utils::PreviewLayout{
                                                      0, 88, 240, 144},
                                                  rect, 0, 88);
  ExpectNear("touch top-left clamps center x", center.x, 0.25f);
  ExpectNear("touch top-left clamps center y", center.y, 0.25f);

  center = pdf_view_utils::RecenterViewportFromPreview(
      pdf_view_utils::PreviewLayout{0, 88, 240, 144}, rect, 240, 232);
  ExpectNear("touch bottom-right clamps center x", center.x, 0.75f);
  ExpectNear("touch bottom-right clamps center y", center.y, 0.75f);
}

void TestTouchMovementThreshold() {
  ExpectTrue("touch movement threshold rejects tiny motion",
             !pdf_view_utils::TouchMovementExceedsThreshold(100, 100, 102, 101,
                                                            3));
  ExpectTrue("touch movement threshold accepts exact threshold",
             pdf_view_utils::TouchMovementExceedsThreshold(100, 100, 103, 100,
                                                           3));
  ExpectTrue("touch movement threshold accepts larger motion",
             pdf_view_utils::TouchMovementExceedsThreshold(100, 100, 100, 105,
                                                           3));
}

void TestViewportAtMinimumZoomMatchesScreenAspect() {
  pdf_view_utils::NormalizedRect rect =
      pdf_view_utils::ComputeViewportRect(1000.0f, 2000.0f, 1.0f, 400.0f,
                                          240.0f, 0.5f, 0.5f);
  ExpectNear("minimum zoom uses full portrait page width", rect.width, 1.0f);
  ExpectNear("minimum zoom crops portrait height to screen aspect", rect.height,
             0.3f);
  ExpectNear("minimum zoom portrait left", rect.left, 0.0f);
  ExpectNear("minimum zoom portrait top", rect.top, 0.35f);
  ExpectNear("minimum zoom crop aspect matches screen",
             (rect.width * 1000.0f) / (rect.height * 2000.0f),
             400.0f / 240.0f);

  rect = pdf_view_utils::ComputeViewportRect(1000.0f, 600.0f, 1.0f,
                                             400.0f, 240.0f, 0.5f, 0.5f);
  ExpectNear("matching page aspect keeps full width", rect.width, 1.0f);
  ExpectNear("matching page aspect keeps full height", rect.height, 1.0f);
}

void TestViewportStateFeedsGeometry() {
  using namespace fixed_layout_viewport_utils;
  ViewportState state; state.max_zoom_index=6; state.zoom_index=5;
  state.center_x=0.2f; state.center_y=0.8f; state.interaction_active=true;
  ResetViewportForTargetChange(&state, 2);
  ExpectEq("target change returns fit zoom", state.zoom_index, 2);
  ExpectTrue("target change ends fast interaction", !state.interaction_active);
  auto rect=pdf_view_utils::ComputeViewportRect(1000, 2000,
      pdf_view_utils::ZoomForIndex(state.zoom_index), 400, 240, state.center_x, state.center_y);
  ExpectNear("target reset centers visible vertical crop", rect.top, 0.35f);
  ExpectNear("target reset keeps whole portrait width", rect.width, 1);
  ExpectTrue("zoom advances", AdjustZoom(&state, 3));
  ResetViewport(&state, PAGE_TURN_RIGHT_TO_LEFT);
  rect=pdf_view_utils::ComputeViewportRect(1000, 2000,
      pdf_view_utils::ZoomForIndex(state.zoom_index), 400, 240, state.center_x, state.center_y);
  ExpectNear("RTL starts at right edge", rect.left + rect.width, 1);
  ExpectNear("page navigation starts at top", rect.top, 0);
  ExpectTrue("page navigation ends fast interaction", !state.interaction_active);
  ResetViewport(&state);
  rect=pdf_view_utils::ComputeViewportRect(1000, 2000,
      pdf_view_utils::ZoomForIndex(state.zoom_index), 400, 240, state.center_x, state.center_y);
  ExpectNear("LTR starts at left edge", rect.left, 0);
  ExpectTrue("pan reaches bottom-right boundary", PanViewport(&state, 2, 2));
  rect=pdf_view_utils::ComputeViewportRect(1000, 2000,
      pdf_view_utils::ZoomForIndex(state.zoom_index), 400, 240, state.center_x, state.center_y);
  ExpectNear("pan clamps visible right edge", rect.left + rect.width, 1);
  ExpectNear("pan clamps visible bottom edge", rect.top + rect.height, 1);
  ExpectTrue("further movement at boundary is idle", !PanViewport(&state, 1, 1));
}
} // namespace

int main() {
  TestViewportStateFeedsGeometry();
  TestZoomPresets();
  TestDevicePolicies();
  TestPreviewFit();
  TestPreviewFitInsideInsetBounds();
  TestViewportClampAndTouchRecenter();
  TestTouchMovementThreshold();
  TestViewportAtMinimumZoomMatchesScreenAspect();
  return 0;
}
