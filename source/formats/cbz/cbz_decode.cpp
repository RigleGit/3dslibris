#include "formats/cbz/cbz_decode.h"

#include "formats/common/pdf_view_utils.h"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

static const int kCbzTopScreenWidth = 240;
static const int kCbzTopScreenHeight = 400;
static char g_last_cbz_decode_error[192] = "";

inline bool ValidBitmapSize(int width, int height) {
  return width > 0 && height > 0;
}

void SetLastCbzDecodeError(const char *message) {
  if (!message)
    message = "";
  std::snprintf(g_last_cbz_decode_error, sizeof(g_last_cbz_decode_error), "%s",
                message);
}

inline uint16_t RGB565FromU8(float r, float g, float b) {
  if (r < 0.0f)
    r = 0.0f;
  else if (r > 255.0f)
    r = 255.0f;
  if (g < 0.0f)
    g = 0.0f;
  else if (g > 255.0f)
    g = 255.0f;
  if (b < 0.0f)
    b = 0.0f;
  else if (b > 255.0f)
    b = 255.0f;
  return ((uint16_t)(r / 8.0f) << 11) | ((uint16_t)(g / 4.0f) << 5) |
         (uint16_t)(b / 8.0f);
}

inline void UnpackRgb565(uint16_t pixel, int *r, int *g, int *b) {
  const int r5 = (pixel >> 11) & 0x1F;
  const int g6 = (pixel >> 5) & 0x3F;
  const int b5 = pixel & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

inline int ClampInt(int value, int lo, int hi) {
  if (value < lo)
    return lo;
  if (value > hi)
    return hi;
  return value;
}

#ifndef __3DS__
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
        const int src_x = std::min(src_width - 1,
                                   (x * src_width) / std::max(1, dst_width));
        const int src_y =
            std::min(src_height - 1, (y * src_height) / std::max(1, dst_height));
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
                                  int max_zoom_index, CbzDecodedPage *out) {
  if (!out || bytes.empty()) {
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

  ctx = fz_new_context(NULL, &g_mupdf_locks_ctx, FZ_STORE_DEFAULT);
  if (!ctx) {
    SetLastCbzDecodeError("fz_new_context failed");
    return false;
  }

  fz_try(ctx) {
    buffer = fz_new_buffer_from_copied_data(ctx, bytes.data(), bytes.size());
    image = fz_new_image_from_buffer(ctx, buffer);
    if (!image || image->w <= 0 || image->h <= 0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid image dimensions");

    out->original_width = image->w;
    out->original_height = image->h;

    const float fit_scale =
        std::min((float)kCbzTopScreenWidth / std::max(1, image->w),
                 (float)kCbzTopScreenHeight / std::max(1, image->h));
    const float zoom = pdf_view_utils::ZoomForIndex(max_zoom_index);
    const float source_scale =
        std::min(1.0f, std::max(0.0001f, fit_scale * zoom));
    const int target_width = ClampInt(
        (int)std::floor(image->w * source_scale + 0.5f), 1, image->w);
    const int target_height = ClampInt(
        (int)std::floor(image->h * source_scale + 0.5f), 1, image->h);

    int l2factor = 0;
    while ((image->w >> (l2factor + 1)) >= target_width + 2 &&
           (image->h >> (l2factor + 1)) >= target_height + 2 &&
           l2factor < 6) {
      l2factor++;
    }

    pixmap = image->get_pixmap(ctx, image, NULL, target_width, target_height,
                               &l2factor);
    if (!pixmap)
      fz_throw(ctx, FZ_ERROR_FORMAT, "image->get_pixmap failed");

    if (l2factor > 0)
      fz_subsample_pixmap(ctx, pixmap, l2factor);

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);
    if (!samples || pix_w <= 0 || pix_h <= 0 || stride <= 0 || comps < 1)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid mupdf image pixmap");

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

    ok = true;
  }
  fz_catch(ctx) {
    SetLastCbzDecodeError(fz_caught_message(ctx));
    out->original_width = 0;
    out->original_height = 0;
    out->source_bitmap.width = 0;
    out->source_bitmap.height = 0;
    out->source_bitmap.pixels.clear();
    ok = false;
  }

  fz_drop_pixmap(ctx, pixmap);
  fz_drop_image(ctx, image);
  fz_drop_buffer(ctx, buffer);
  fz_drop_context(ctx);
  return ok;
}

#endif

} // namespace

bool ScaleCbzBitmap(const CbzBitmap &src, int dst_width, int dst_height,
                    bool high_quality, CbzBitmap *out) {
  if (!out || !ValidBitmapSize(src.width, src.height) ||
      src.pixels.empty() || !ValidBitmapSize(dst_width, dst_height)) {
    SetLastCbzDecodeError("invalid bitmap scale parameters");
    return false;
  }

  out->width = dst_width;
  out->height = dst_height;
  out->pixels.assign((size_t)dst_width * (size_t)dst_height, 0);

  for (int y = 0; y < dst_height; y++) {
    for (int x = 0; x < dst_width; x++) {
      if (!high_quality) {
        const int src_x = std::min(src.width - 1,
                                   (x * src.width) / std::max(1, dst_width));
        const int src_y =
            std::min(src.height - 1, (y * src.height) / std::max(1, dst_height));
        out->pixels[(size_t)y * (size_t)dst_width + (size_t)x] =
            src.pixels[(size_t)src_y * (size_t)src.width + (size_t)src_x];
        continue;
      }

      const float src_xf =
          (((float)x + 0.5f) * (float)src.width / (float)dst_width) - 0.5f;
      const float src_yf =
          (((float)y + 0.5f) * (float)src.height / (float)dst_height) - 0.5f;
      const float clamped_x =
          std::max(0.0f, std::min((float)(src.width - 1), src_xf));
      const float clamped_y =
          std::max(0.0f, std::min((float)(src.height - 1), src_yf));
      const int x0 = (int)clamped_x;
      const int y0 = (int)clamped_y;
      const int x1 = std::min(src.width - 1, x0 + 1);
      const int y1 = std::min(src.height - 1, y0 + 1);
      const float tx = clamped_x - (float)x0;
      const float ty = clamped_y - (float)y0;

      int r00 = 0, g00 = 0, b00 = 0;
      int r10 = 0, g10 = 0, b10 = 0;
      int r01 = 0, g01 = 0, b01 = 0;
      int r11 = 0, g11 = 0, b11 = 0;
      UnpackRgb565(src.pixels[(size_t)y0 * (size_t)src.width + (size_t)x0], &r00,
                   &g00, &b00);
      UnpackRgb565(src.pixels[(size_t)y0 * (size_t)src.width + (size_t)x1], &r10,
                   &g10, &b10);
      UnpackRgb565(src.pixels[(size_t)y1 * (size_t)src.width + (size_t)x0], &r01,
                   &g01, &b01);
      UnpackRgb565(src.pixels[(size_t)y1 * (size_t)src.width + (size_t)x1], &r11,
                   &g11, &b11);

      const float w00 = (1.0f - tx) * (1.0f - ty);
      const float w10 = tx * (1.0f - ty);
      const float w01 = (1.0f - tx) * ty;
      const float w11 = tx * ty;

      out->pixels[(size_t)y * (size_t)dst_width + (size_t)x] = RGB565FromU8(
          r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 0.5f,
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
                        int max_zoom_index, CbzDecodedPage *out) {
  if (!out || bytes.empty())
    return false;
  SetLastCbzDecodeError("");

#ifdef __3DS__
  return DecodeImageToBitmapWithMuPdf(bytes, max_zoom_index, out);
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

  const float fit_scale =
      std::min((float)kCbzTopScreenWidth / std::max(1, src_width),
               (float)kCbzTopScreenHeight / std::max(1, src_height));
  const float zoom = pdf_view_utils::ZoomForIndex(max_zoom_index);
  const float source_scale = std::min(1.0f, std::max(0.0001f, fit_scale * zoom));
  const int target_width =
      ClampInt((int)std::floor(src_width * source_scale + 0.5f), 1, src_width);
  const int target_height =
      ClampInt((int)std::floor(src_height * source_scale + 0.5f), 1, src_height);

#ifdef __3DS__
  if (IsJpegBuffer(bytes)) {
    return DecodeJpegScaledToBitmap(bytes, src_width, src_height, target_width,
                                    target_height, &out->source_bitmap);
  }
#endif

  unsigned char *decoded = stbi_load_from_memory(
      bytes.data(), (int)bytes.size(), &src_width, &src_height, &src_components,
      3);
  if (!decoded) {
    SetLastCbzDecodeError("stbi_load failed");
    return false;
  }

  const bool ok =
      ResampleRgb8ToBitmap(decoded, src_width, src_height, 3, target_width,
                           target_height, true, &out->source_bitmap);
  stbi_image_free(decoded);
  return ok;
#endif
}
