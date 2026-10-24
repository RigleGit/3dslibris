#include "formats/cbz/cbz_decode.h"

#include "formats/common/fixed_layout_screen_constants.h"
#include "formats/common/pdf_view_utils.h"
#include "shared/color_utils.h"
#include "shared/fixed_layout_perf.h"
#include "shared/stb_image_gif_utils.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdint.h>

#ifdef __3DS__
#include "formats/mupdf/mupdf_common.h"

extern "C" {
#include "mupdf/fitz/buffer.h"
#include "mupdf/fitz/image.h"
}
#endif

namespace {

static char g_last_cbz_decode_error[192] = "";

struct CbzDecodeTargetSize {
  int width;
  int height;
};

inline bool ValidBitmapSize(int width, int height) {
  return width > 0 && height > 0;
}

void SetLastCbzDecodeError(const char *message) {
  if (!message)
    message = "";
  std::snprintf(g_last_cbz_decode_error, sizeof(g_last_cbz_decode_error), "%s",
                message);
}

void ClearDecodedPage(CbzDecodedPage *out) {
  if (!out)
    return;

  out->original_width = 0;
  out->original_height = 0;
  out->source_bitmap.width = 0;
  out->source_bitmap.height = 0;
  out->source_bitmap.pixels.clear();
}

inline int ClampInt(int value, int lo, int hi) {
  if (value < lo)
    return lo;
  if (value > hi)
    return hi;
  return value;
}

CbzDecodeTargetSize ComputeDecodeTargetSize(int src_width, int src_height,
                                            int max_zoom_index,
                                            int target_width,
                                            int target_height) {
  CbzDecodeTargetSize target;
  target.width = 1;
  target.height = 1;

  if (!ValidBitmapSize(src_width, src_height))
    return target;

  const float fit_scale =
      std::min((float)target_width / (float)src_width,
               (float)target_height /
                   (float)src_height);
  const float zoom = pdf_view_utils::ZoomForIndex(max_zoom_index);
  const float source_scale =
      std::min(1.0f, std::max(0.0001f, fit_scale * zoom));

  target.width =
      ClampInt((int)std::floor(src_width * source_scale + 0.5f), 1, src_width);
  target.height = ClampInt((int)std::floor(src_height * source_scale + 0.5f), 1,
                           src_height);
  return target;
}

#ifndef __3DS__

struct ScopedStbiImage {
  explicit ScopedStbiImage(unsigned char *ptr) : data(ptr) {}

  ~ScopedStbiImage() {
    if (data)
      stbi_image_free(data);
  }

  unsigned char *data;

private:
  ScopedStbiImage(const ScopedStbiImage &);
  ScopedStbiImage &operator=(const ScopedStbiImage &);
};

bool ResampleRgb8ToBitmap(const unsigned char *src, int src_width,
                          int src_height, int src_components, int dst_width,
                          int dst_height, bool high_quality, CbzBitmap *out) {
  if (!src || !out || !ValidBitmapSize(src_width, src_height) ||
      !ValidBitmapSize(dst_width, dst_height) || src_components < 3) {
    SetLastCbzDecodeError("invalid resample parameters");
    return false;
  }

  out->width = dst_width;
  out->height = dst_height;
  out->pixels.assign((size_t)dst_width * (size_t)dst_height, 0);

  for (int y = 0; y < dst_height; y++) {
    for (int x = 0; x < dst_width; x++) {
      if (!high_quality) {
        const int src_x =
            std::min(src_width - 1, (x * src_width) / std::max(1, dst_width));
        const int src_y = std::min(src_height - 1,
                                   (y * src_height) / std::max(1, dst_height));
        const unsigned char *p =
            src + ((size_t)src_y * (size_t)src_width + (size_t)src_x) *
                      (size_t)src_components;

        out->pixels[(size_t)y * (size_t)dst_width + (size_t)x] =
            RGB565FromU8((float)p[0], (float)p[1], (float)p[2]);
        continue;
      }

      const float src_xf =
          (((float)x + 0.5f) * (float)src_width / (float)dst_width) - 0.5f;
      const float src_yf =
          (((float)y + 0.5f) * (float)src_height / (float)dst_height) - 0.5f;
      const float clamped_x =
          std::max(0.0f, std::min((float)(src_width - 1), src_xf));
      const float clamped_y =
          std::max(0.0f, std::min((float)(src_height - 1), src_yf));

      const int x0 = (int)clamped_x;
      const int y0 = (int)clamped_y;
      const int x1 = std::min(src_width - 1, x0 + 1);
      const int y1 = std::min(src_height - 1, y0 + 1);
      const float tx = clamped_x - (float)x0;
      const float ty = clamped_y - (float)y0;

      const unsigned char *p00 =
          src + ((size_t)y0 * (size_t)src_width + (size_t)x0) *
                    (size_t)src_components;
      const unsigned char *p10 =
          src + ((size_t)y0 * (size_t)src_width + (size_t)x1) *
                    (size_t)src_components;
      const unsigned char *p01 =
          src + ((size_t)y1 * (size_t)src_width + (size_t)x0) *
                    (size_t)src_components;
      const unsigned char *p11 =
          src + ((size_t)y1 * (size_t)src_width + (size_t)x1) *
                    (size_t)src_components;

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;

      const float r = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
      const float g = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
      const float b = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;

      out->pixels[(size_t)y * (size_t)dst_width + (size_t)x] =
          RGB565FromU8(r, g, b);
    }
  }

  return true;
}

#endif

#ifdef __3DS__

bool DecodeImageToBitmapWithMuPdf(const std::vector<unsigned char> &bytes,
                                  int max_zoom_index, int target_width,
                                  int target_height, CbzDecodedPage *out) {
  if (!out || bytes.empty()) {
    ClearDecodedPage(out);
    SetLastCbzDecodeError("invalid mupdf image decode parameters");
    return false;
  }

  InitMuPdfLocks();

  fz_context *ctx = NULL;
  fz_buffer *buffer = NULL;
  fz_image *image = NULL;
  fz_pixmap *pixmap = NULL;
  bool ok = false;

  fz_var(buffer);
  fz_var(image);
  fz_var(pixmap);

  const uint64_t context_start = fixed_perf::Now();
  ctx = fz_new_context(NULL, &g_mupdf_locks_ctx, FZ_STORE_DEFAULT);
  fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_context",
                     fixed_perf::Now() - context_start, ctx ? 1 : 0);
  if (!ctx) {
    ClearDecodedPage(out);
    SetLastCbzDecodeError("fz_new_context failed");
    return false;
  }

  fz_try(ctx) {
    const uint64_t buffer_start = fixed_perf::Now();
    // All MuPDF references, including the context's store, are dropped below
    // before this synchronous call returns. Borrow the caller's immutable
    // entry bytes; the shared buffer never frees the vector's storage.
    buffer = fz_new_buffer_from_shared_data(ctx, bytes.data(), bytes.size());
    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_buffer",
                       fixed_perf::Now() - buffer_start, 1, bytes.size());
    const uint64_t header_start = fixed_perf::Now();
    image = fz_new_image_from_buffer(ctx, buffer);

    if (!image || image->w <= 0 || image->h <= 0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid image dimensions");

    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_header",
                       fixed_perf::Now() - header_start, 1, 0, image->w, image->h);
    out->original_width = image->w;
    out->original_height = image->h;

    const CbzDecodeTargetSize target =
        ComputeDecodeTargetSize(image->w, image->h, max_zoom_index,
                                target_width, target_height);

    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_target", 0, 1,
                       0, target.width, target.height);
    int l2factor = 0;
    while ((image->w >> (l2factor + 1)) >= target.width + 2 &&
           (image->h >> (l2factor + 1)) >= target.height + 2 && l2factor < 6) {
      l2factor++;
    }

    const uint64_t pixmap_start = fixed_perf::Now();
    pixmap = image->get_pixmap(ctx, image, NULL, target.width, target.height,
                               &l2factor);
    if (!pixmap)
      fz_throw(ctx, FZ_ERROR_FORMAT, "image->get_pixmap failed");

    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_pixmap",
                       fixed_perf::Now() - pixmap_start, 1, 0,
                       fz_pixmap_width(ctx, pixmap), fz_pixmap_height(ctx, pixmap));
    const uint64_t subsample_start = fixed_perf::Now();
    if (l2factor > 0)
      fz_subsample_pixmap(ctx, pixmap, l2factor);
    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_subsample",
                       fixed_perf::Now() - subsample_start, 1, 0,
                       fz_pixmap_width(ctx, pixmap), fz_pixmap_height(ctx, pixmap));

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);

    if (!samples || pix_w <= 0 || pix_h <= 0 || stride <= 0 || comps < 1)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid mupdf image pixmap");

    const uint64_t rgb_start = fixed_perf::Now();
    out->source_bitmap.width = pix_w;
    out->source_bitmap.height = pix_h;
    out->source_bitmap.pixels.resize((size_t)pix_w * (size_t)pix_h);

    uint16_t *dst = out->source_bitmap.pixels.data();

    if (comps == 1 || comps == 2) {
      for (int y = 0; y < pix_h; y++) {
        const unsigned char *src = samples + (size_t)y * (size_t)stride;
        for (int x = 0; x < pix_w; x++) {
          *dst++ = RGB565FromRgb8(src[0], src[0], src[0]);
          src += comps;
        }
      }
    } else {
      for (int y = 0; y < pix_h; y++) {
        const unsigned char *src = samples + (size_t)y * (size_t)stride;
        for (int x = 0; x < pix_w; x++) {
          *dst++ = RGB565FromRgb8(src[0], src[1], src[2]);
          src += comps;
        }
      }
    }

    fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_rgb565",
                       fixed_perf::Now() - rgb_start, 1,
                       out->source_bitmap.pixels.size() * sizeof(uint16_t),
                       pix_w, pix_h);
    ok = true;
  }
  fz_catch(ctx) {
    SetLastCbzDecodeError(fz_convert_error(ctx, NULL));
    ClearDecodedPage(out);
    ok = false;
  }

  const uint64_t release_start = fixed_perf::Now();
  fz_drop_pixmap(ctx, pixmap);
  fz_drop_image(ctx, image);
  fz_drop_buffer(ctx, buffer);
  fz_drop_context(ctx);
  fixed_perf::Record(&bytes, -1, max_zoom_index, "cbz.decode_release",
                     fixed_perf::Now() - release_start, ok ? 1 : 0);

  return ok;
}

#endif

} // namespace

bool ScaleCbzBitmap(const CbzBitmap &src, int dst_width, int dst_height,
                    bool high_quality, CbzBitmap *out) {
  if (!out || !ValidBitmapSize(src.width, src.height) || src.pixels.empty() ||
      !ValidBitmapSize(dst_width, dst_height)) {
    SetLastCbzDecodeError("invalid bitmap scale parameters");
    return false;
  }

  if (src.width == dst_width && src.height == dst_height) {
    if (out != &src)
      *out = src;
    return true;
  }

  out->width = dst_width;
  out->height = dst_height;
  out->pixels.assign((size_t)dst_width * (size_t)dst_height, 0);

  if (!high_quality) {
    // Compute each column once instead of dividing for every pixel on ARM11.
    // The temporary table costs four bytes per output column, not per pixel.
    std::vector<int> source_x((size_t)dst_width);
    for (int x = 0; x < dst_width; x++)
      source_x[x] = (x * src.width) / dst_width;
    for (int y = 0; y < dst_height; y++) {
      const int src_y = (y * src.height) / dst_height;
      const uint16_t *row = &src.pixels[(size_t)src_y * src.width];
      uint16_t *dst = &out->pixels[(size_t)y * dst_width];
      for (int x = 0; x < dst_width; x++)
        dst[x] = row[source_x[x]];
    }
    return true;
  }

  struct ColumnSample {
    int x0, x1;
    float t;
  };
  std::vector<ColumnSample> columns((size_t)dst_width);
  for (int x = 0; x < dst_width; x++) {
    const float src_xf =
        (((float)x + 0.5f) * (float)src.width / (float)dst_width) - 0.5f;
    const float clamped_x =
        std::max(0.0f, std::min((float)(src.width - 1), src_xf));
    columns[x].x0 = (int)clamped_x;
    columns[x].x1 = std::min(src.width - 1, columns[x].x0 + 1);
    columns[x].t = clamped_x - (float)columns[x].x0;
  }
  for (int y = 0; y < dst_height; y++) {
    const float src_yf =
        (((float)y + 0.5f) * (float)src.height / (float)dst_height) - 0.5f;
    const float clamped_y =
        std::max(0.0f, std::min((float)(src.height - 1), src_yf));
    const int y0 = (int)clamped_y;
    const int y1 = std::min(src.height - 1, y0 + 1);
    const float ty = clamped_y - (float)y0;
    for (int x = 0; x < dst_width; x++) {
      const int x0 = columns[x].x0;
      const int x1 = columns[x].x1;
      const float tx = columns[x].t;

      int r00 = 0, g00 = 0, b00 = 0;
      int r10 = 0, g10 = 0, b10 = 0;
      int r01 = 0, g01 = 0, b01 = 0;
      int r11 = 0, g11 = 0, b11 = 0;

      UnpackRgb565(src.pixels[(size_t)y0 * (size_t)src.width + (size_t)x0],
                   &r00, &g00, &b00);
      UnpackRgb565(src.pixels[(size_t)y0 * (size_t)src.width + (size_t)x1],
                   &r10, &g10, &b10);
      UnpackRgb565(src.pixels[(size_t)y1 * (size_t)src.width + (size_t)x0],
                   &r01, &g01, &b01);
      UnpackRgb565(src.pixels[(size_t)y1 * (size_t)src.width + (size_t)x1],
                   &r11, &g11, &b11);

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;

      out->pixels[(size_t)y * (size_t)dst_width + (size_t)x] =
          RGB565FromU8(r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 0.5f,
                       g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 0.5f,
                       b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 0.5f);
    }
  }

  return true;
}

const char *GetLastCbzDecodeError() {
  return g_last_cbz_decode_error;
}

bool DecodeCbzPageImage(const std::vector<unsigned char> &bytes,
                        int max_zoom_index, int target_width,
                        int target_height, CbzDecodedPage *out) {
  if (!out)
    return false;

  ClearDecodedPage(out);
  SetLastCbzDecodeError("");

  if (bytes.empty()) {
    SetLastCbzDecodeError("empty image buffer");
    return false;
  }

#ifdef __3DS__
  return DecodeImageToBitmapWithMuPdf(bytes, max_zoom_index, target_width,
                                      target_height, out);
#else
  int src_width = 0;
  int src_height = 0;
  int src_components = 0;

  if (!stbi_info_from_memory(bytes.data(), (int)bytes.size(), &src_width,
                             &src_height, &src_components) ||
      !ValidBitmapSize(src_width, src_height)) {
    SetLastCbzDecodeError("stbi_info failed");
    return false;
  }

  out->original_width = src_width;
  out->original_height = src_height;

  const CbzDecodeTargetSize target =
      ComputeDecodeTargetSize(src_width, src_height, max_zoom_index,
                              target_width, target_height);

  ScopedStbiImage decoded(stb_image_gif_utils::LoadFromMemory(
      bytes.data(), (int)bytes.size(), &src_width, &src_height, &src_components,
      3));

  if (!decoded.data) {
    ClearDecodedPage(out);
    SetLastCbzDecodeError("stbi_load failed");
    return false;
  }

  return ResampleRgb8ToBitmap(decoded.data, src_width, src_height, 3,
                              target.width, target.height, true,
                              &out->source_bitmap);
#endif
}
