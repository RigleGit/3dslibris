#pragma once
#include "formats/common/fixed_layout_gpu_renderer.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/pdf_view_utils.h"
#include <algorithm>

namespace fixed_layout_gpu {
// Synchronous bridge used only by the draw owners. A decoder lends its pixmap
// for one call; neither a worker nor the texture cache retains those samples.
struct DecodedUpload {
  Text *text;
  uint64_t generation;
  int preview_box_width, preview_box_height;
  TextureCache *replaced;
  TextureCache texture;
  std::vector<uint16_t> preview;
  int preview_width = 0, preview_height = 0;
  bool attempted = false;

  DecodedUpload(Text *t, uint64_t gen, int w, int h, TextureCache *old = nullptr)
      : text(t), generation(gen), preview_box_width(w), preview_box_height(h),
        replaced(old) {}
  raster_image::Rgb8Sink Sink() { return {this, Consume}; }

  static bool Consume(void *opaque, const raster_image::Rgb8View &source,
                       float width, float height) {
    DecodedUpload &out = *static_cast<DecodedUpload *>(opaque);
    if (!out.text || !source.Valid())
      return false;
    if (out.preview_box_width > 0 && out.preview_box_height > 0) {
      const auto layout = pdf_view_utils::ComputePreviewLayout(
          width, height, out.preview_box_width, out.preview_box_height);
      out.preview_width = std::max(1, layout.width);
      out.preview_height = std::max(1, layout.height);
      out.preview.resize((size_t)out.preview_width * out.preview_height);
      if (!fixed_layout_blit_utils::ScaleRgb8Preview(
              source, out.preview_width, out.preview_height, out.preview.data()))
        return false;
    }
    // Release the previous page only after the new pixmap exists, and before
    // asking VRAM/Linear for its replacement. A failed upload still has the
    // decoder's live samples available for the normal CPU conversion.
    if (out.replaced)
      out.replaced->Reset();
    // A second consume is a new decoded raster (for example the smaller
    // visible-area retry after an expanded allocation fails). Keep the draw
    // cache's per-frame failure latch, but allow this bounded decoder retry.
    if (out.attempted)
      out.texture.Reset();
    out.attempted = true;
    return out.texture.UploadRgb(out.text, source, out.generation);
  }
};
} // namespace fixed_layout_gpu
