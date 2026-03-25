#include "shared/pdf_view_utils.h"

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

void TestZoomPresets() {
  ExpectEq("zoom index below range clamps", pdf_view_utils::ClampZoomIndex(-4),
           0);
  ExpectEq("zoom index above range clamps", pdf_view_utils::ClampZoomIndex(99),
           4);
  ExpectEq("default zoom index", pdf_view_utils::DefaultZoomIndex(), 2);
  ExpectNear("zoom preset 1.0", pdf_view_utils::ZoomForIndex(0), 1.0f);
  ExpectNear("zoom preset 2.0", pdf_view_utils::ZoomForIndex(2), 2.0f);
  ExpectNear("zoom preset 4.0", pdf_view_utils::ZoomForIndex(4), 4.0f);
}

void TestPreviewFit() {
  pdf_view_utils::PreviewLayout fit =
      pdf_view_utils::ComputePreviewLayout(1000.0f, 2000.0f, 240, 320);
  ExpectEq("preview x centers portrait page", fit.x, 40);
  ExpectEq("preview y portrait starts at top", fit.y, 0);
  ExpectEq("preview width portrait", fit.width, 160);
  ExpectEq("preview height portrait", fit.height, 320);
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

void TestViewportAtFitZoomCoversWholePage() {
  pdf_view_utils::NormalizedRect rect =
      pdf_view_utils::ComputeViewportRect(1000.0f, 2000.0f, 1.0f, 400.0f,
                                          240.0f, 0.5f, 0.5f);
  ExpectNear("fit zoom width covers page", rect.width, 1.0f);
  ExpectNear("fit zoom height covers page", rect.height, 1.0f);
  ExpectNear("fit zoom left", rect.left, 0.0f);
  ExpectNear("fit zoom top", rect.top, 0.0f);
}

} // namespace

int main() {
  TestZoomPresets();
  TestPreviewFit();
  TestViewportClampAndTouchRecenter();
  TestViewportAtFitZoomCoversWholePage();
  return 0;
}
