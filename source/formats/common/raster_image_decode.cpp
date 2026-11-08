#include "formats/common/raster_image_decode.h"
#include "shared/color_utils.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

#ifdef __3DS__
#include <3ds.h>
#endif

#define RASTER_STR2(x) #x
#define RASTER_STR(x) RASTER_STR2(x)
#define RASTER_DECODER_LIMIT_MIB_STR RASTER_STR(RASTER_DECODER_LIMIT_MIB)
namespace {
// Header preserves the alignment required by MuPDF and records realloc sizes
// plus which pool owns the block (newlib heap or, on 3DS, linear memory).
struct AllocationHeader {
  union {
    size_t size;
    long double alignment;
    void *pointer;
  };
  bool linear;
};
struct Budget {
  size_t used = 0, peak = 0;
  bool limit_hit = false;     // 32 MiB decoder budget exceeded
  bool out_of_memory = false; // no pool could satisfy the request
};
// Newlib's heap is small and fragments over a long session (Old 3DS), so a
// decode that no longer fits there falls back to the unused linear pool.
AllocationHeader *RawAlloc(Budget &b, size_t bytes) {
  AllocationHeader *p = static_cast<AllocationHeader *>(std::malloc(bytes));
  if (p) {
    p->linear = false;
    return p;
  }
#ifdef __3DS__
  p = static_cast<AllocationHeader *>(linearAlloc(bytes));
  if (p) {
    p->linear = true;
  }
#else
  (void)b;
#endif
  return p;
}
void RawFree(AllocationHeader *p) {
#ifdef __3DS__
  if (p->linear) {
    linearFree(p);
    return;
  }
#endif
  std::free(p);
}
void *Allocate(void *opaque, size_t size) {
  Budget &b = *static_cast<Budget *>(opaque);
  if (size > raster_image::kDecoderLimit - sizeof(AllocationHeader) ||
      b.used > raster_image::kDecoderLimit - size - sizeof(AllocationHeader)) {
    b.limit_hit = true;
    return NULL;
  }
  AllocationHeader *p = RawAlloc(b, sizeof(*p) + size);
  if (!p) {
    b.out_of_memory = true;
    return NULL;
  }
  p->size = size + sizeof(*p);
  b.used += p->size;
  b.peak = std::max(b.peak, b.used);
  return p + 1;
}
void Release(void *opaque, void *ptr) {
  if (!ptr)
    return;
  AllocationHeader *p = static_cast<AllocationHeader *>(ptr) - 1;
  static_cast<Budget *>(opaque)->used -= p->size;
  RawFree(p);
}
void *Resize(void *opaque, void *ptr, size_t size) {
  if (!ptr)
    return Allocate(opaque, size);
  if (!size) {
    Release(opaque, ptr);
    return NULL;
  }
  Budget &b = *static_cast<Budget *>(opaque);
  AllocationHeader *p = static_cast<AllocationHeader *>(ptr) - 1;
  const size_t old = p->size;
  if (size > raster_image::kDecoderLimit - sizeof(*p) ||
      b.used - old > raster_image::kDecoderLimit - size - sizeof(*p)) {
    b.limit_hit = true;
    return NULL;
  }
  AllocationHeader *q = NULL;
  if (!p->linear)
    q = static_cast<AllocationHeader *>(std::realloc(p, size + sizeof(*p)));
  if (q) {
    q->linear = false;
  } else if (p->linear || size + sizeof(*p) > old) {
    // Heap realloc failed (p is still valid) or the block is linear: move.
    q = RawAlloc(b, size + sizeof(*p));
    if (!q) {
      b.out_of_memory = true;
      return NULL;
    }
    std::memcpy(q + 1, p + 1, std::min(old - sizeof(*p), size));
    RawFree(p);
  } else {
    q = p; // shrinking a linear block in place
  }
  q->size = size + sizeof(*p);
  b.used = b.used - old + q->size;
  b.peak = std::max(b.peak, b.used);
  return q + 1;
}
} // namespace

#ifdef __3DS__
#include "formats/mupdf/mupdf_common.h"
extern "C" {
#include "mupdf/fitz/buffer.h"
#include "mupdf/fitz/image.h"
}
// MuPDF composes all GIF frames. The book viewer promises the first frame,
// matching inline images, so use a private bounded stb GIF decoder instead.
#define STBI_ONLY_GIF
#else
// Host reference decoder; this does not validate the ARM MuPDF JPEG/PNG path.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#endif
static Budget *stbi_budget;
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MALLOC(n) Allocate(stbi_budget, n)
#define STBI_REALLOC(p, n) Resize(stbi_budget, p, n)
#define STBI_FREE(p) Release(stbi_budget, p)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace raster_image {
namespace {
size_t bitmap_bytes = 0;
}
void FreePixels::operator()(uint16_t *p) const {
  if (!p)
    return;
  AllocationHeader *header = reinterpret_cast<AllocationHeader *>(p) - 1;
  bitmap_bytes -= header->size;
  std::free(header);
}
bool Bitmap::Allocate(int w, int h) {
  if (w <= 0 || h <= 0 || (size_t)w > kBitmapLimit / sizeof(uint16_t) / h)
    return false;
  const size_t bytes = (size_t)w * h * sizeof(uint16_t);
  // Bound transient replacement too: at most 8 MiB across bitmaps, with
  // each enlarged bitmap capped at 4 MiB. This preserves a valid old view.
  if (bytes > 2 * kBitmapLimit - bitmap_bytes)
    return false;
  AllocationHeader *header =
      static_cast<AllocationHeader *>(std::malloc(sizeof(*header) + bytes));
  if (!header)
    return false;
  header->size = bytes;
  header->linear = false;
  bitmap_bytes += bytes;
  pixels.reset(reinterpret_cast<uint16_t *>(header + 1));
  width = w;
  height = h;
  return true;
}

namespace {
template <typename Output>
bool Target(int source_w, int source_h, int requested_w, int requested_h,
            Output *bitmap, bool exact_size) {
  if (source_w <= 0 || source_h <= 0 || requested_w <= 0 || requested_h <= 0)
    return false;
  const double scale = std::min(1.0, std::max((double)requested_w / source_w,
                                              (double)requested_h / source_h));
  bitmap->width = exact_size ? requested_w : std::max(1, (int)(source_w * scale + 0.5));
  bitmap->height = exact_size ? requested_h : std::max(1, (int)(source_h * scale + 0.5));
  if (!bitmap->Allocate(bitmap->width, bitmap->height))
    return false;
  bitmap->original_width = source_w;
  bitmap->original_height = source_h;
  return true;
}

void StorePixel(Bitmap *out,size_t index,float rgb[3],float alpha,const int bg[3]) {
  if (alpha != 255.0f)
    for(int c=0;c<3;++c) rgb[c]+=bg[c]*(1.0f-alpha/255.0f);
  out->pixels.get()[index]=RGB565FromU8(rgb[0]+0.5f,rgb[1]+0.5f,rgb[2]+0.5f);
}
void StorePixel(ArgbBitmap *out,size_t index,float rgb[3],float alpha,const int[3]) {
  const unsigned a=std::max(0,std::min(255,(int)(alpha+0.5f)));
  uint32_t pixel=a<<24;
  const float unpremultiply = alpha > 0 ? 255.0f / alpha : 0.0f;
  if(alpha>0) for(int c=0;c<3;++c) {
    const float value_rgb = rgb[c] * unpremultiply;
    const unsigned value=std::max(0,std::min(255,(int)(value_rgb+0.5f)));
    pixel|=value<<((2-c)*8);
  }
  out->pixels.mutable_data()[index]=pixel;
}

// Resample premultiplied colors, then retain alpha or compose onto the background.
// This avoids dark fringes around transparent pixels when reducing images.
template <typename Output>
void Convert(const unsigned char *samples, int width, int height, int stride,
             int components, bool alpha, bool premultiplied,
             uint16_t background, Output *out) {
  const int colors = components - (alpha ? 1 : 0);
  const int bg[3] = {((background >> 11) & 31) * 255 / 31,
                     ((background >> 5) & 63) * 255 / 63,
                     (background & 31) * 255 / 31};
  if (width == out->width && height == out->height) {
    for (int y = 0; y < height; ++y) {
      const unsigned char *src = samples + (size_t)y * stride;
      for (int x = 0; x < width; ++x, src += components) {
        const int opacity = alpha ? src[components - 1] : 255;
        const float factor = premultiplied || opacity == 255 ? 1.0f : opacity / 255.0f;
        float rgb[3];
        for (int c = 0; c < 3; ++c)
          rgb[c] = src[colors == 1 ? 0 : c] * factor;
        StorePixel(out, (size_t)y * width + x, rgb, (float)opacity, bg);
      }
    }
    return;
  }
  struct ColumnSample { int x0, x1; float t; };
  auto column_sample = [&](int x) -> ColumnSample {
    const float fx = std::max(0.0f, ((x + 0.5f) * width / out->width) - 0.5f);
    ColumnSample sample;
    sample.x0 = std::min(width - 1, (int)fx);
    sample.x1 = std::min(width - 1, sample.x0 + 1);
    sample.t = fx - sample.x0;
    return sample;
  };
  // Bound scratch memory even for a very wide, one-row input. Failure keeps
  // the original per-pixel coordinate calculation available.
  const size_t kMaxColumnSamples = 4096;
  image_memory::Buffer<ColumnSample> columns;
  const bool cached_columns = (size_t)out->width <= kMaxColumnSamples &&
      columns.resize((size_t)out->width, kMaxColumnSamples * sizeof(ColumnSample));
  if (cached_columns) {
    for (int x = 0; x < out->width; ++x)
      columns.mutable_data()[x] = column_sample(x);
  }
  for (int y = 0; y < out->height; ++y) {
    const float fy = std::max(0.0f, ((y + 0.5f) * height / out->height) - 0.5f);
    const int y0 = std::min(height - 1, (int)fy),
              y1 = std::min(height - 1, y0 + 1);
    const float ty = fy - y0;
    for (int x = 0; x < out->width; ++x) {
      const ColumnSample column = cached_columns ? columns[x] : column_sample(x);
      const int x0 = column.x0, x1 = column.x1;
      const float tx = column.t;
      const unsigned char *p[4] = {
          samples + (size_t)y0 * stride + (size_t)x0 * components,
          samples + (size_t)y0 * stride + (size_t)x1 * components,
          samples + (size_t)y1 * stride + (size_t)x0 * components,
          samples + (size_t)y1 * stride + (size_t)x1 * components};
      const float weights[4] = {(1 - tx) * (1 - ty), tx * (1 - ty),
                                (1 - tx) * ty, tx * ty};
      float rgb[3] = {0, 0, 0}, a = 0;
      bool opaque = true;
      for (int k = 0; k < 4; ++k) {
        const int opacity = alpha ? p[k][components - 1] : 255;
        opaque = opaque && opacity == 255;
        a += weights[k] * opacity;
        const float factor = premultiplied ? 1.0f : opacity / 255.0f;
        for (int c = 0; c < 3; ++c)
          rgb[c] += weights[k] * p[k][colors == 1 ? 0 : c] * factor;
      }
      StorePixel(out,(size_t)y*out->width+x,rgb,opaque ? 255.0f : a,bg);
    }
  }
}
} // namespace

namespace {
template <typename Output>
bool DecodeWithStbi(const unsigned char *bytes,size_t length,int target_width,
                    int target_height, uint16_t background, Output *out,
                    Budget *budget, bool exact_size) {
  stbi_budget = budget;
  int w = 0, h = 0, channels = 0;
  bool ok = false;
  if (stbi_info_from_memory(bytes, (int)length, &w, &h,
                            &channels) &&
      Target(w, h, target_width, target_height, out, exact_size)) {
    unsigned char *pixels = stbi_load_from_memory(
        bytes, (int)length, &w, &h, &channels, 4);
    if (pixels) {
      Convert(pixels, w, h, w * 4, 4, true, false, background, out);
      ok = true;
    }
    stbi_image_free(pixels);
  }
  stbi_budget = NULL;
  return ok;
}
} // namespace

template <typename Output>
bool DecodeImage(const unsigned char *bytes,size_t length,int target_width,
            int target_height, uint16_t background, Output *out,
            const char **error, bool exact_size = false) {
  if (error)
    *error = "Image missing or unsupported";
  if (!out || !bytes || !length || length > INT_MAX || target_width <= 0 ||
      target_height <= 0)
    return false;
  Output next;
  Budget budget;
  bool ok = false;
#ifdef __3DS__
  const bool is_gif =
      length >= 6 && std::memcmp(bytes, "GIF8", 4) == 0;
  if (is_gif) {
    ok = DecodeWithStbi(bytes, length, target_width, target_height, background, &next,
                        &budget, exact_size);
  } else {
    InitMuPdfLocks();
    fz_alloc_context allocator = {&budget, Allocate, Resize, Release};
    fz_context *ctx =
        fz_new_context(&allocator, &g_mupdf_locks_ctx, 1024u * 1024u);
    if (!ctx) {
      if (error)
        *error = "Decoder memory limit";
      return false;
    }
    fz_buffer *buffer = NULL;
    fz_image *image = NULL;
    fz_pixmap *pixmap = NULL;
    fz_var(buffer);
    fz_var(image);
    fz_var(pixmap);
    fz_var(ok);
    fz_var(next);
    fz_var(budget);
    fz_try(ctx) {
      buffer = fz_new_buffer_from_shared_data(ctx, bytes, length);
      image = fz_new_image_from_buffer(ctx, buffer);
      if (!image ||
          !Target(image->w, image->h, target_width, target_height, &next, exact_size))
        fz_throw(ctx, FZ_ERROR_LIMIT, "Bitmap memory limit");
      int factor = SubsampleFactor(image->w, image->h, next.width, next.height);
      pixmap =
          image->get_pixmap(ctx, image, NULL, next.width, next.height, &factor);
      if (!pixmap)
        fz_throw(ctx, FZ_ERROR_FORMAT, "Image decode failed");
      if (factor > 0)
        fz_subsample_pixmap(ctx, pixmap, factor);
      int components = fz_pixmap_components(ctx, pixmap);
      bool alpha = fz_pixmap_alpha(ctx, pixmap) != 0;
      if (components - (alpha ? 1 : 0) != 1 &&
          components - (alpha ? 1 : 0) != 3) {
        fz_pixmap *rgb =
            fz_convert_pixmap(ctx, pixmap, fz_device_rgb(ctx), NULL, NULL,
                              fz_default_color_params, 1);
        fz_drop_pixmap(ctx, pixmap);
        pixmap = rgb;
        components = fz_pixmap_components(ctx, pixmap);
        alpha = fz_pixmap_alpha(ctx, pixmap) != 0;
      }
      // MuPDF treats a decoder error (e.g. a progressive JPEG whose coefficient
      // buffers exceed the budget) as end of data and returns a zero-filled
      // pixmap. Refuse that pixmap instead of presenting it as a black image.
      if (budget.limit_hit || budget.out_of_memory)
        fz_throw(ctx, FZ_ERROR_LIMIT, "Decoder memory limit");
      Convert(fz_pixmap_samples(ctx, pixmap), fz_pixmap_width(ctx, pixmap),
              fz_pixmap_height(ctx, pixmap), fz_pixmap_stride(ctx, pixmap),
              components, alpha, true, background, &next);
      ok = true;
    }
    fz_catch(ctx) {
      ok = false;
    }
    fz_drop_pixmap(ctx, pixmap);
    fz_drop_image(ctx, image);
    fz_drop_buffer(ctx, buffer);
    fz_drop_context(ctx);
  }
#else
  ok = DecodeWithStbi(bytes, length, target_width, target_height, background, &next,
                      &budget, exact_size);
#endif
  if (!ok) {
    if (error)
      *error = budget.limit_hit
                   ? "Decoder memory limit (" RASTER_DECODER_LIMIT_MIB_STR " MiB)"
                   : budget.out_of_memory ? "Decoder out of memory"
                                          : "Image corrupt or bitmap limit";
    return false;
  }
  next.decoder_peak = budget.peak;
  *out = std::move(next);
  if (error)
    *error = NULL;
  return true;
}
bool Decode(const unsigned char *bytes,size_t length,int w,int h,
            uint16_t bg,Bitmap *out,const char **error) {
  return DecodeImage(bytes,length,w,h,bg,out,error);
}
bool DecodeArgb(const unsigned char *bytes,size_t length,int w,int h,
                ArgbBitmap *out,const char **error, bool exact_size) {
  return DecodeImage(bytes,length,w,h,0,out,error,exact_size);
}
} // namespace raster_image
