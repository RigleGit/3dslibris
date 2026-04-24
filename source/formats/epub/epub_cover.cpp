/*
    3dslibris - epub_cover.cpp
    EPUB cover image extraction and decoding.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_cover.h"

#include "book/book.h"
#include "debug_log.h"
#include "formats/common/book_error.h"
#include "formats/common/epub_image_utils.h"
#include "formats/epub/epub.h"
#include "formats/epub/epub_cover_decode_utils.h"
#include "formats/epub/epub_limits.h"
#include "formats/mupdf/mupdf_common.h"
#include "minizip/unzip.h"
#include "path_utils.h"
#include "stb_image.h"
#include "shared/string_utils.h"
#include <png.h>
#include <3ds.h>
#include <algorithm>
#include <ctype.h>
#include <setjmp.h>
#include <vector>

extern "C" {
#include "mupdf/fitz.h"
}

namespace {

static const int kCoverThumbMaxW = 85;
static const int kCoverThumbMaxH = 115;

static std::string BuildDocPath(const std::string &opf_folder,
                                const std::string &href) {
  if (opf_folder.empty())
    return NormalizePath(UrlDecode(href));
  return NormalizePath(opf_folder + "/" + UrlDecode(href));
}

static bool FindManifestItemPath(epub_data_t &data, const std::string &id,
                                 const std::string &opf_folder,
                                 std::string &path_out) {
  for (auto item : data.manifest) {
    if (item->id == id) {
      path_out = BuildDocPath(opf_folder, item->href);
      return true;
    }
  }
  return false;
}

static bool LocateZipEntrySafe(unzFile uf, const std::string &entry_path) {
  if (!uf || entry_path.empty())
    return false;

  if (unzLocateFile(uf, entry_path.c_str(), 0) == UNZ_OK)
    return true;

  std::string wanted = NormalizeZipEntryName(entry_path);
  int rc = unzGoToFirstFile(uf);
  if (rc != UNZ_OK)
    return false;

  do {
    unz_file_info fi;
    char fname[1024];
    int info_rc =
        unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL, 0);
    if (info_rc == UNZ_OK) {
      std::string current = NormalizeZipEntryName(std::string(fname));
      if (current == wanted || EqualsAsciiNoCase(current, wanted))
        return true;
    }
    rc = unzGoToNextFile(uf);
  } while (rc == UNZ_OK);

  return false;
}

static bool ResolveSvgCoverPayload(unzFile uf, const std::string &svg_path,
                                   const std::vector<u8> &svg_buf,
                                   std::vector<u8> *out,
                                   std::string *resolved_path) {
  if (!out || svg_buf.empty() ||
      svg_buf.size() > epub_limits::kSvgWrapperMaxBytes)
    return false;
  return epub_image_utils::ResolveSvgWrapperImage(
      uf, svg_path, svg_buf, out, epub_limits::kCoverMaxEntryBytes,
      resolved_path);
}

static bool LooksLikePngPayload(const std::string &path_hint,
                                const std::vector<u8> &buf) {
  std::string lower_path = ToLowerAscii(path_hint);
  if (lower_path.size() >= 4 &&
      lower_path.rfind(".png") == lower_path.size() - 4) {
    return true;
  }
  return buf.size() >= 8 && !png_sig_cmp((png_bytep)buf.data(), 0, 8);
}

static bool AdoptCoverPixels(Book *book, const std::vector<u16> &pixels, int w,
                             int h) {
  if (!book || pixels.empty() || w <= 0 || h <= 0 ||
      pixels.size() != (size_t)w * (size_t)h) {
    return false;
  }
  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
  book->coverPixels = new u16[(size_t)w * (size_t)h];
  if (!book->coverPixels)
    return false;
  memcpy(book->coverPixels, pixels.data(), pixels.size() * sizeof(u16));
  book->coverWidth = w;
  book->coverHeight = h;
  return true;
}

struct PngMemoryReader {
  const unsigned char *data;
  size_t size;
  size_t offset;
};

static void ReadPngFromMemory(png_structp png_ptr, png_bytep out_bytes,
                              png_size_t byte_count) {
  PngMemoryReader *reader =
      (PngMemoryReader *)png_get_io_ptr(png_ptr);
  if (!reader || reader->offset + byte_count > reader->size) {
    png_error(png_ptr, "png read overflow");
    return;
  }
  memcpy(out_bytes, reader->data + reader->offset, byte_count);
  reader->offset += (size_t)byte_count;
}

static bool DecodePngCoverThumbnail(const std::vector<u8> &decodebuf,
                                    std::vector<u16> *thumb_pixels,
                                    int *thumb_w_out, int *thumb_h_out) {
  if (!thumb_pixels || !thumb_w_out || !thumb_h_out ||
      !LooksLikePngPayload("", decodebuf)) {
    return false;
  }

  png_structp png_ptr =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr)
    return false;
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_read_struct(&png_ptr, NULL, NULL);
    return false;
  }

  PngMemoryReader reader = {decodebuf.data(), decodebuf.size(), 0};
  bool ok = false;
  std::vector<u16> thumb;
  std::vector<png_byte> row_buf;

  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return false;
  }

  png_set_read_fn(png_ptr, &reader, ReadPngFromMemory);
  png_read_info(png_ptr, info_ptr);

  png_uint_32 img_w = 0;
  png_uint_32 img_h = 0;
  int bit_depth = 0;
  int color_type = 0;
  png_get_IHDR(png_ptr, info_ptr, &img_w, &img_h, &bit_depth, &color_type,
               NULL, NULL, NULL);

  if (img_w == 0 || img_h == 0 || img_w > epub_limits::kCoverMaxDimension ||
      img_h > epub_limits::kCoverMaxDimension) {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return false;
  }

  if (bit_depth == 16)
    png_set_strip_16(png_ptr);
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png_ptr);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png_ptr);
  if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png_ptr);
  if (color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png_ptr);
  }
  if (color_type & PNG_COLOR_MASK_ALPHA)
    png_set_strip_alpha(png_ptr);

  png_read_update_info(png_ptr, info_ptr);

  const int channels = png_get_channels(png_ptr, info_ptr);
  const png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
  if (channels < 3 || rowbytes == 0) {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return false;
  }

  int thumb_w = 0;
  int thumb_h = 0;
  float scale = 1.0f;
  if (!epub_cover_decode_utils::ComputeCoverThumbSize(
          (int)img_w, (int)img_h, kCoverThumbMaxW, kCoverThumbMaxH, &thumb_w,
          &thumb_h, &scale)) {
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return false;
  }

  thumb.assign((size_t)thumb_w * (size_t)thumb_h, 0);
  row_buf.resize(rowbytes);

  std::vector<int> sample_x((size_t)thumb_w);
  for (int x = 0; x < thumb_w; x++) {
    int src_x = (int)((float)x * scale);
    if (src_x >= (int)img_w)
      src_x = (int)img_w - 1;
    sample_x[(size_t)x] = std::max(0, src_x);
  }

  int out_y = 0;
  int target_src_y = 0;
  for (png_uint_32 y = 0; y < img_h; y++) {
    png_read_row(png_ptr, row_buf.data(), NULL);
    while (out_y < thumb_h && target_src_y == (int)y) {
      for (int x = 0; x < thumb_w; x++) {
        const png_bytep px = row_buf.data() + (size_t)sample_x[(size_t)x] *
                                                  (size_t)channels;
        const u16 r = (u16)((px[0] >> 3) & 0x1F);
        const u16 g = (u16)((px[1] >> 2) & 0x3F);
        const u16 b = (u16)((px[2] >> 3) & 0x1F);
        thumb[(size_t)out_y * (size_t)thumb_w + (size_t)x] =
            (u16)((r << 11) | (g << 5) | b);
      }
      out_y++;
      if (out_y < thumb_h) {
        target_src_y = (int)((float)out_y * scale);
        if (target_src_y >= (int)img_h)
          target_src_y = (int)img_h - 1;
      }
    }
  }

  png_read_end(png_ptr, NULL);
  png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

  *thumb_pixels = thumb;
  *thumb_w_out = thumb_w;
  *thumb_h_out = thumb_h;
  return true;
}

static bool RenderSvgCoverThumbnail(const std::vector<u8> &svg_buf,
                                    std::vector<u16> *thumb_pixels,
                                    int *thumb_w_out, int *thumb_h_out) {
  if (!thumb_pixels || !thumb_w_out || !thumb_h_out || svg_buf.empty())
    return false;

  fz_context *ctx = NULL;
  fz_buffer *buf = NULL;
  fz_document *doc = NULL;
  fz_page *page = NULL;
  fz_pixmap *pixmap = NULL;
  fz_device *device = NULL;
  bool ok = false;

  InitMuPdfLocks();
  ctx = fz_new_context(NULL, &g_mupdf_locks_ctx, FZ_STORE_DEFAULT);
  if (!ctx)
    return false;
  fz_register_document_handlers(ctx);

  fz_var(buf);
  fz_var(doc);
  fz_var(page);
  fz_var(pixmap);
  fz_var(device);

  fz_try(ctx) {
    buf = fz_new_buffer_from_copied_data(ctx, svg_buf.data(), svg_buf.size());
    doc = fz_open_document_with_buffer(ctx, "image/svg+xml", buf);
    if (!doc)
      fz_throw(ctx, FZ_ERROR_FORMAT, "failed to open svg document");
    page = fz_load_page(ctx, doc, 0);
    if (!page)
      fz_throw(ctx, FZ_ERROR_FORMAT, "failed to load svg page");

    const fz_rect page_bounds = fz_bound_page(ctx, page);
    float svg_w = 0.0f;
    float svg_h = 0.0f;
    svg_w = page_bounds.x1 - page_bounds.x0;
    svg_h = page_bounds.y1 - page_bounds.y0;
    if (!(svg_w > 0.0f && svg_h > 0.0f))
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid svg size");

    int thumb_w = 0;
    int thumb_h = 0;
    float scale = 1.0f;
    if (!epub_cover_decode_utils::ComputeCoverThumbSize(
            (int)(svg_w + 0.5f), (int)(svg_h + 0.5f), kCoverThumbMaxW,
            kCoverThumbMaxH, &thumb_w, &thumb_h, &scale)) {
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid svg thumbnail size");
    }

    const fz_rect bounds = fz_make_rect(0.0f, 0.0f, svg_w, svg_h);
    const fz_matrix ctm = fz_scale(scale, scale);
    const fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
    if (bbox.x1 <= bbox.x0 || bbox.y1 <= bbox.y0)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid svg bbox");

    pixmap = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, NULL, 0);
    fz_clear_pixmap_with_value(ctx, pixmap, 255);
    device = fz_new_draw_device(ctx, fz_identity, pixmap);
    fz_run_page(ctx, page, device, ctm, NULL);
    fz_close_device(ctx, device);

    const int pix_w = fz_pixmap_width(ctx, pixmap);
    const int pix_h = fz_pixmap_height(ctx, pixmap);
    const int stride = fz_pixmap_stride(ctx, pixmap);
    const int comps = fz_pixmap_components(ctx, pixmap);
    unsigned char *samples = fz_pixmap_samples(ctx, pixmap);
    if (!samples || pix_w <= 0 || pix_h <= 0 || comps < 3)
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid svg pixmap");

    thumb_pixels->assign((size_t)pix_w * (size_t)pix_h, 0);
    for (int y = 0; y < pix_h; y++) {
      const unsigned char *src = samples + (size_t)y * (size_t)stride;
      for (int x = 0; x < pix_w; x++) {
        const u16 r = (u16)((src[0] >> 3) & 0x1F);
        const u16 g = (u16)((src[1] >> 2) & 0x3F);
        const u16 b = (u16)((src[2] >> 3) & 0x1F);
        (*thumb_pixels)[(size_t)y * (size_t)pix_w + (size_t)x] =
            (u16)((r << 11) | (g << 5) | b);
        src += comps;
      }
    }
    *thumb_w_out = pix_w;
    *thumb_h_out = pix_h;
    ok = true;
  }
  fz_always(ctx) {
    fz_drop_device(ctx, device);
    fz_drop_pixmap(ctx, pixmap);
    fz_drop_page(ctx, page);
    fz_drop_document(ctx, doc);
    fz_drop_buffer(ctx, buf);
  }
  fz_catch(ctx) { ok = false; }
  fz_drop_context(ctx);

  return ok;
}

static bool DecodeImageCoverThumbnailWithMuPdf(const std::vector<u8> &decodebuf,
                                               std::vector<u16> *thumb_pixels,
                                               int *thumb_w_out,
                                               int *thumb_h_out) {
  if (!thumb_pixels || !thumb_w_out || !thumb_h_out || decodebuf.empty())
    return false;

  fz_context *ctx = NULL;
  fz_buffer *buf = NULL;
  fz_image *image = NULL;
  fz_pixmap *pixmap = NULL;
  bool ok = false;

  InitMuPdfLocks();
  ctx = fz_new_context(NULL, &g_mupdf_locks_ctx, FZ_STORE_DEFAULT);
  if (!ctx)
    return false;

  fz_var(buf);
  fz_var(image);
  fz_var(pixmap);

  fz_try(ctx) {
    buf = fz_new_buffer_from_copied_data(ctx, decodebuf.data(),
                                         decodebuf.size());
    image = fz_new_image_from_buffer(ctx, buf);
    if (!image || image->w <= 0 || image->h <= 0 ||
        image->w > epub_limits::kCoverMaxDimension ||
        image->h > epub_limits::kCoverMaxDimension) {
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid cover image dimensions");
    }

    int thumb_w = 0;
    int thumb_h = 0;
    float scale = 1.0f;
    if (!epub_cover_decode_utils::ComputeCoverThumbSize(
            image->w, image->h, kCoverThumbMaxW, kCoverThumbMaxH, &thumb_w,
            &thumb_h, &scale)) {
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid cover thumbnail size");
    }

    int l2factor = epub_cover_decode_utils::ComputeJpegL2SubsampleFactor(
        image->w, image->h, thumb_w, thumb_h);
    pixmap = image->get_pixmap(ctx, image, NULL, thumb_w, thumb_h, &l2factor);
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
      fz_throw(ctx, FZ_ERROR_FORMAT, "invalid cover pixmap");

    thumb_pixels->assign((size_t)thumb_w * (size_t)thumb_h, 0);
    for (int y = 0; y < thumb_h; y++) {
      int src_y = (int)((long long)y * (long long)pix_h /
                        (long long)thumb_h);
      if (src_y >= pix_h)
        src_y = pix_h - 1;
      const unsigned char *row = samples + (size_t)src_y * (size_t)stride;
      for (int x = 0; x < thumb_w; x++) {
        int src_x = (int)((long long)x * (long long)pix_w /
                          (long long)thumb_w);
        if (src_x >= pix_w)
          src_x = pix_w - 1;
        const unsigned char *px = row + (size_t)src_x * (size_t)comps;
        const unsigned char r = px[0];
        const unsigned char g = (comps >= 3) ? px[1] : px[0];
        const unsigned char b = (comps >= 3) ? px[2] : px[0];
        (*thumb_pixels)[(size_t)y * (size_t)thumb_w + (size_t)x] =
            (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
    }
    *thumb_w_out = thumb_w;
    *thumb_h_out = thumb_h;
    ok = true;
  }
  fz_catch(ctx) { ok = false; }

  fz_drop_pixmap(ctx, pixmap);
  fz_drop_image(ctx, image);
  fz_drop_buffer(ctx, buf);
  fz_drop_context(ctx);
  return ok;
}

} // namespace

namespace epub_cover {

int Extract(Book *book, const std::string &epubpath) {
  if (!book || book->coverImagePath.empty())
    return 1;

  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return 2;

  if (!LocateZipEntrySafe(uf, book->coverImagePath)) {
    unzClose(uf);
    return 3;
  }

  unz_file_info fi;
  unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0);

  if (fi.uncompressed_size == 0 ||
      fi.uncompressed_size > epub_limits::kCoverMaxEntryBytes ||
      fi.uncompressed_size > (uLong)INT_MAX) {
    unzClose(uf);
    return 8;
  }

  std::vector<u8> imgbuf((size_t)fi.uncompressed_size);
  int rc = unzOpenCurrentFile(uf);
  if (rc != UNZ_OK) {
    unzClose(uf);
    return 5;
  }

  int total = 0;
  while (total < (int)imgbuf.size()) {
    if ((book->GetStatusReporter() &&
         book->GetStatusReporter()->ShouldAbortWork()) ||
        book->IsOpenAbortRequested()) {
      unzCloseCurrentFile(uf);
      unzClose(uf);
      return BOOK_ERR_CANCELLED;
    }
    int n = unzReadCurrentFile(uf, imgbuf.data() + total,
                               (unsigned int)(imgbuf.size() - (size_t)total));
    if (n < 0) {
      unzCloseCurrentFile(uf);
      unzClose(uf);
      return 5;
    }
    if (n == 0)
      break;
    total += n;
  }
  unzCloseCurrentFile(uf);
  if (total <= 0) {
    unzClose(uf);
    return 5;
  }
  imgbuf.resize((size_t)total);

  std::vector<u8> decodebuf = imgbuf;
  std::string decode_path = book->coverImagePath;
  const bool is_svg_cover =
      epub_image_utils::LooksLikeSvgWrapper(decode_path, decodebuf);
  if (epub_image_utils::LooksLikeSvgWrapper(decode_path, decodebuf)) {
    std::vector<u8> resolved;
    std::string resolved_path;
    if (ResolveSvgCoverPayload(uf, decode_path, decodebuf, &resolved,
                               &resolved_path)) {
      decodebuf.swap(resolved);
      if (!resolved_path.empty())
        decode_path = resolved_path;
    }
  }
  unzClose(uf);

  if (is_svg_cover && decode_path == book->coverImagePath) {
    std::vector<u16> svg_pixels;
    int svg_w = 0;
    int svg_h = 0;
    if (RenderSvgCoverThumbnail(decodebuf, &svg_pixels, &svg_w, &svg_h) &&
        AdoptCoverPixels(book, svg_pixels, svg_w, svg_h)) {
      return 0;
    }
  }

  auto IsJpegCover = [&]() -> bool {
    if (decode_path.size() >= 4) {
      std::string lower = decode_path;
      for (size_t i = 0; i < lower.size(); i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
      if (lower.rfind(".jpg") == lower.size() - 4 ||
          lower.rfind(".jpeg") == lower.size() - 5)
        return true;
    }
    return decodebuf.size() >= 3 && decodebuf[0] == 0xFF &&
           decodebuf[1] == 0xD8 && decodebuf[2] == 0xFF;
  };

  if (decodebuf.size() > epub_limits::kCoverMaxNonJpegBytes && !IsJpegCover())
    return 8;

  if ((book->GetStatusReporter() &&
       book->GetStatusReporter()->ShouldAbortWork()) ||
      book->IsOpenAbortRequested())
    return BOOK_ERR_CANCELLED;

  const bool jpeg_cover = IsJpegCover();
  int infoW = 0, infoH = 0, infoChannels = 0;
  bool hasInfo = stbi_info_from_memory(decodebuf.data(), (int)decodebuf.size(),
                                       &infoW, &infoH, &infoChannels) != 0;
  if (!hasInfo && decodebuf.size() > epub_limits::kCoverMaxNonJpegBytes)
    return 9;

  if (hasInfo) {
    if (infoW <= 0 || infoH <= 0 || infoW > epub_limits::kCoverMaxDimension ||
        infoH > epub_limits::kCoverMaxDimension)
      return 7;
    size_t decoded_bytes = (size_t)infoW * (size_t)infoH * 3;
    if (decoded_bytes > epub_limits::kCoverMaxDecodedRgbBytes)
      return 9;
    if (LooksLikePngPayload(decode_path, decodebuf) &&
        decoded_bytes > 4u * 1024u * 1024u) {
      std::vector<u16> png_pixels;
      int png_w = 0;
      int png_h = 0;
      if (DecodePngCoverThumbnail(decodebuf, &png_pixels, &png_w, &png_h) &&
          AdoptCoverPixels(book, png_pixels, png_w, png_h)) {
        return 0;
      }
    }
  }

  if (jpeg_cover) {
    std::vector<u16> jpg_pixels;
    int jpg_w = 0;
    int jpg_h = 0;
    if (DecodeImageCoverThumbnailWithMuPdf(decodebuf, &jpg_pixels, &jpg_w,
                                           &jpg_h) &&
        AdoptCoverPixels(book, jpg_pixels, jpg_w, jpg_h)) {
      return 0;
    }
  }

  int imgW, imgH, channels;
  unsigned char *pixels =
      stbi_load_from_memory(decodebuf.data(), (int)decodebuf.size(), &imgW,
                            &imgH, &channels, 3);

  if (!pixels) {
    if (LooksLikePngPayload(decode_path, decodebuf)) {
      std::vector<u16> png_pixels;
      int png_w = 0;
      int png_h = 0;
      if (DecodePngCoverThumbnail(decodebuf, &png_pixels, &png_w, &png_h) &&
          AdoptCoverPixels(book, png_pixels, png_w, png_h)) {
        return 0;
      }
    }
    return 4;
  }

  if (imgW <= 0 || imgH <= 0 || imgW > epub_limits::kCoverMaxDimension ||
      imgH > epub_limits::kCoverMaxDimension) {
    stbi_image_free(pixels);
    return 7;
  }

  int finalW = 0;
  int finalH = 0;
  float scale = 1.0f;
  if (!epub_cover_decode_utils::ComputeCoverThumbSize(
          imgW, imgH, kCoverThumbMaxW, kCoverThumbMaxH, &finalW, &finalH,
          &scale)) {
    stbi_image_free(pixels);
    return 7;
  }

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
  book->coverPixels = new u16[finalW * finalH];
  book->coverWidth = finalW;
  book->coverHeight = finalH;

  for (int y = 0; y < finalH; y++) {
    int srcY = (int)(y * scale);
    if (srcY >= imgH)
      srcY = imgH - 1;
    for (int x = 0; x < finalW; x++) {
      int srcX = (int)(x * scale);
      if (srcX >= imgW)
        srcX = imgW - 1;
      unsigned char *px = &pixels[(srcY * imgW + srcX) * 3];
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * finalW + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return 0;
}

bool FindLikelyImagePath(epub_data_t &data, const std::string &opf_folder,
                         std::string &path_out) {
  if (!data.coverid.empty()) {
    for (auto item : data.manifest) {
      if (item && item->id == data.coverid) {
        if (item->media_type.find("image/") == 0) {
          path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
          return true;
        }
        break;
      }
    }
  }

  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    if (ContainsNoCase(item->id, "cover") ||
        ContainsNoCase(item->href, "cover") ||
        ContainsNoCase(item->href, "portada") ||
        ContainsNoCase(item->properties, "cover")) {
      path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
      return true;
    }
  }

  for (auto item : data.manifest) {
    if (!item || item->media_type.find("image/") != 0)
      continue;
    path_out = NormalizePath(opf_folder + "/" + UrlDecode(item->href));
    return true;
  }

  path_out.clear();
  return false;
}

} // namespace epub_cover
