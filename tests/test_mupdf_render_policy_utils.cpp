#include "formats/mupdf/mupdf_render_policy_utils.h"

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

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void TestSkipPdfPageRenderDetectsComplexPages() {
  using app_flow_utils::MuPdfDocumentKind;
  ExpectTrue("skips many xobjects",
             mupdf_render_policy_utils::ShouldSkipPdfPageRender(
                 MuPdfDocumentKind::Pdf,
                 mupdf_render_policy_utils::kOld3dsPdfPreviewMaxXObjects + 1,
                 1024));
  ExpectTrue("skips large content stream",
             mupdf_render_policy_utils::ShouldSkipPdfPageRender(
                 MuPdfDocumentKind::Pdf, 1,
                 mupdf_render_policy_utils::kOld3dsPdfPreviewMaxContentBytes +
                     1));
  ExpectFalse("keeps simple pdf",
              mupdf_render_policy_utils::ShouldSkipPdfPageRender(
                  MuPdfDocumentKind::Pdf, 4, 32u * 1024u));
  ExpectFalse("keeps non-pdf",
              mupdf_render_policy_utils::ShouldSkipPdfPageRender(
                  MuPdfDocumentKind::Xps, 1000, 1024u * 1024u));
  ExpectFalse("keeps exact complexity limits",
              mupdf_render_policy_utils::ShouldSkipPdfPageRender(
                  MuPdfDocumentKind::Pdf,
                  mupdf_render_policy_utils::kOld3dsPdfPreviewMaxXObjects,
                  mupdf_render_policy_utils::kOld3dsPdfPreviewMaxContentBytes));

}

} // namespace

int main() {
  TestSkipPdfPageRenderDetectsComplexPages();
  return 0;
}
