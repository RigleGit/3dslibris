#pragma once

#include "shared/app_flow_utils.h"

#include <cstddef>

namespace mupdf_render_policy_utils {

static const int kOld3dsPdfPreviewMaxXObjects = 128;
static const size_t kOld3dsPdfPreviewMaxContentBytes = 192u * 1024u;

// Device-agnostic check: applies the same complexity thresholds to all
// hardware. Use this for any PDF render path (preview, cover extraction, etc.)
// regardless of whether the device is old or new 3DS.
inline bool ShouldSkipPdfPageRender(
    app_flow_utils::MuPdfDocumentKind document_kind, int xobject_count,
    size_t content_bytes) {
  if (document_kind != app_flow_utils::MuPdfDocumentKind::Pdf)
    return false;
  return xobject_count > kOld3dsPdfPreviewMaxXObjects ||
         content_bytes > kOld3dsPdfPreviewMaxContentBytes;
}

} // namespace mupdf_render_policy_utils
