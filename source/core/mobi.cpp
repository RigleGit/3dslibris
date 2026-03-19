/*
    3dslibris - mobi.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - MOBI cover extraction from PDB records (EXTH hints + image record scan).
    - JPEG/PNG/GIF/BMP decode to RGB565 cover thumbnail for browser cache.
*/

#include "mobi.h"

#include "app.h"
#include "book_error.h"
#include "file_read_utils.h"
#include "mobi_cover_meta_cache.h"
#include "mobi_record_scan.h"
#include "stb_image.h"
#include "string_utils.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace {

static const size_t kMobiCoverFileMaxBytes = 64 * 1024 * 1024;
static const size_t kMobiCoverRecordMaxBytes = 8 * 1024 * 1024;
static const int kMobiCoverMaxDimension = 2048;
static const char *kMobiCoverMetaCacheBaseDir = "sdmc:/3ds/3dslibris/cache";
static const char *kMobiCoverMetaCacheDir = "sdmc:/3ds/3dslibris/cache/mobi-cover";

struct MobiCoverCandidate {
  u32 record_idx;
  size_t image_off;
  size_t image_len;
  int width;
  int height;
  float score;
};

static bool DecodeAndScaleToCover(Book *book, const u8 *data, size_t size);

static void EnsureMobiCoverMetaCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kMobiCoverMetaCacheBaseDir, 0777);
  mkdir(kMobiCoverMetaCacheDir, 0777);
  initialized = true;
}

static bool BuildMobiCoverMetaCachePath(const std::string &mobipath,
                                        std::string *out) {
  if (!out || mobipath.empty())
    return false;
  struct stat st;
  if (stat(mobipath.c_str(), &st) != 0)
    return false;
  EnsureMobiCoverMetaCacheDirs();
  *out = mobi_cover_meta_cache::BuildPath(
      mobipath, (long long)st.st_size, (long long)st.st_mtime);
  return !out->empty();
}

static bool ReadFileSlice(const std::string &path, u32 start, u32 end,
                          std::vector<u8> *out) {
  if (!out || path.empty() || end <= start)
    return false;
  out->clear();
  size_t len = (size_t)(end - start);
  if (len == 0 || len > kMobiCoverRecordMaxBytes)
    return false;

  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
    return false;
  bool ok = false;
  if (fseek(fp, (long)start, SEEK_SET) == 0) {
    out->resize(len);
    ok = fread(out->data(), 1, len, fp) == len;
  }
  fclose(fp);
  if (!ok)
    out->clear();
  return ok;
}

static bool TryDecodeCachedCoverMeta(Book *book, const std::string &mobipath,
                                     const mobi_cover_meta_cache::CoverMeta &meta,
                                     App *app) {
  if (!book || meta.kind != mobi_cover_meta_cache::kCandidate ||
      meta.record_end <= meta.record_start)
    return false;

  std::vector<u8> record;
  if (!ReadFileSlice(mobipath, meta.record_start, meta.record_end, &record))
    return false;
  if (meta.image_offset >= record.size())
    return false;

  const u8 *image = record.data() + meta.image_offset;
  size_t image_len = record.size() - (size_t)meta.image_offset;
  if (!DecodeAndScaleToCover(book, image, image_len))
    return false;

  if (app) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: cover meta cache hit rec=%u off=%u dim=%ux%u",
             (unsigned)meta.record_index, (unsigned)meta.image_offset,
             (unsigned)meta.width, (unsigned)meta.height);
    app->PrintStatus(msg);
  }
  return true;
}

static void SaveMobiCoverMetaCandidate(
    const std::string &cache_path, const std::vector<u32> &offsets,
    const MobiCoverCandidate &cand) {
  if (cache_path.empty() || cand.record_idx + 1 >= offsets.size())
    return;
  mobi_cover_meta_cache::CoverMeta meta;
  meta.kind = mobi_cover_meta_cache::kCandidate;
  meta.record_index = cand.record_idx;
  meta.record_start = offsets[(size_t)cand.record_idx];
  meta.record_end = offsets[(size_t)cand.record_idx + 1];
  meta.image_offset = (u32)cand.image_off;
  meta.width = (u32)cand.width;
  meta.height = (u32)cand.height;
  mobi_cover_meta_cache::Save(cache_path, meta);
}

static void SaveMobiCoverMetaMiss(const std::string &cache_path) {
  if (cache_path.empty())
    return;
  mobi_cover_meta_cache::CoverMeta meta;
  meta.kind = mobi_cover_meta_cache::kNoCover;
  mobi_cover_meta_cache::Save(cache_path, meta);
}

static u16 ReadBE16(const u8 *p) {
  return (u16)((u16)p[0] << 8 | (u16)p[1]);
}

static u32 ReadBE32(const u8 *p) {
  return (u32)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]);
}

static bool ParseMobiOffsets(const std::string &raw, std::vector<u32> *offsets) {
  if (!offsets || raw.size() < 78)
    return false;

  const u8 *buf = (const u8 *)raw.data();
  const u16 rec_count = ReadBE16(buf + 76);
  if (rec_count == 0 || raw.size() < 78 + (size_t)rec_count * 8)
    return false;

  offsets->assign((size_t)rec_count + 1, 0);
  for (u16 i = 0; i < rec_count; i++) {
    u32 off = ReadBE32(buf + 78 + (size_t)i * 8);
    if (off >= raw.size())
      return false;
    if (i > 0 && off < (*offsets)[(size_t)i - 1])
      return false;
    (*offsets)[i] = off;
  }
  (*offsets)[(size_t)rec_count] = (u32)raw.size();
  return true;
}

static void AddRecordCandidate(std::vector<u32> *out, std::set<u32> *seen,
                               u32 idx, u32 rec_count) {
  if (!out || !seen)
    return;
  if (idx == 0 || idx >= rec_count)
    return;
  if (seen->insert(idx).second)
    out->push_back(idx);
}

static bool ParseMobiCoverHints(const u8 *rec0, size_t rec0_len,
                                u32 *text_rec_count,
                                u32 *first_non_book_index,
                                u32 *first_image_index, u32 *cover_offset,
                                u32 *thumb_offset, u32 *kf8_boundary) {
  if (!rec0 || rec0_len < 24)
    return false;

  if (text_rec_count)
    *text_rec_count = ReadBE16(rec0 + 8);
  if (first_non_book_index)
    *first_non_book_index = 0;
  if (first_image_index)
    *first_image_index = 0;
  if (cover_offset)
    *cover_offset = UINT_MAX;
  if (thumb_offset)
    *thumb_offset = UINT_MAX;
  if (kf8_boundary)
    *kf8_boundary = UINT_MAX;

  if (memcmp(rec0 + 16, "MOBI", 4) != 0)
    return false;

  const u8 *mobi = rec0 + 16;
  const u32 mobi_len = ReadBE32(mobi + 4);
  if (mobi_len < 0x20 || rec0_len < 16 + (size_t)mobi_len)
    return false;

  if (mobi_len >= 0x84 && rec0_len >= 16 + (size_t)0x84 && first_non_book_index)
    *first_non_book_index = ReadBE32(mobi + 0x80);
  if (mobi_len >= 0x70 && rec0_len >= 16 + (size_t)0x70 && first_image_index)
    *first_image_index = ReadBE32(mobi + 0x6C);

  // EXTH records (when bit 0x40 is set in EXTH flags at 0x80)
  if (mobi_len >= 0x84) {
    u32 exth_flags = ReadBE32(mobi + 0x80);
    const bool flagged_exth = (exth_flags & 0x40) != 0;
    const u8 *exth = mobi + mobi_len;
    size_t remain = rec0_len - (size_t)(exth - rec0);
    const bool looks_like_exth =
        (remain >= 12 && memcmp(exth, "EXTH", 4) == 0);
    if (flagged_exth || looks_like_exth) {
      if (looks_like_exth) {
        u32 exth_len = ReadBE32(exth + 4);
        u32 recs = ReadBE32(exth + 8);
        if (exth_len >= 12 && exth_len <= remain) {
          const u8 *p = exth + 12;
          const u8 *end = exth + exth_len;
          for (u32 i = 0; i < recs && p + 8 <= end; i++) {
            u32 type = ReadBE32(p + 0);
            u32 size = ReadBE32(p + 4);
            if (size < 8 || p + size > end)
              break;
            if ((type == 201 || type == 202) && size >= 12) {
              u32 val = ReadBE32(p + 8);
              if (type == 201 && cover_offset)
                *cover_offset = val;
              if (type == 202 && thumb_offset)
                *thumb_offset = val;
            } else if (type == 121 && size >= 12) {
              // EXTH 121 = KF8 boundary record index (combo files).
              if (kf8_boundary)
                *kf8_boundary = ReadBE32(p + 8);
            }
            p += size;
          }
        }
      }
    }
  }

  return true;
}

static bool IsImageSigAt(const u8 *data, size_t len, size_t off) {
  if (!data || off >= len)
    return false;
  if (off + 3 <= len && data[off] == 0xFF && data[off + 1] == 0xD8 &&
      data[off + 2] == 0xFF)
    return true; // JPEG
  if (off + 8 <= len && data[off] == 0x89 && data[off + 1] == 0x50 &&
      data[off + 2] == 0x4E && data[off + 3] == 0x47 && data[off + 4] == 0x0D &&
      data[off + 5] == 0x0A && data[off + 6] == 0x1A && data[off + 7] == 0x0A)
    return true; // PNG
  if (off + 6 <= len &&
      (!memcmp(data + off, "GIF87a", 6) || !memcmp(data + off, "GIF89a", 6)))
    return true; // GIF
  if (off + 2 <= len && data[off] == 'B' && data[off + 1] == 'M')
    return true; // BMP
  return false;
}

static size_t FindImageStartOffset(const u8 *data, size_t len) {
  if (!data || len < 8)
    return SIZE_MAX;
  if (IsImageSigAt(data, len, 0))
    return 0;

  // Some MOBI image records prepend binary wrapper bytes before the real
  // image stream. Scan a larger prefix to recover those covers.
  const size_t probe = std::min((size_t)16384, len);
  for (size_t off = 1; off + 8 <= probe; off++) {
    if (IsImageSigAt(data, len, off))
      return off;
  }
  return SIZE_MAX;
}

static bool DecodeAndScaleToCover(Book *book, const u8 *data, size_t size) {
  if (!book || !data || size == 0 || size > (size_t)INT_MAX)
    return false;

  int info_w = 0, info_h = 0, info_c = 0;
  bool has_info = stbi_info_from_memory(data, (int)size, &info_w, &info_h, &info_c) != 0;
  if (!has_info)
    return false;
  if (info_w <= 0 || info_h <= 0 || info_w > kMobiCoverMaxDimension ||
      info_h > kMobiCoverMaxDimension)
    return false;

  int img_w = 0, img_h = 0, channels = 0;
  unsigned char *pixels =
      stbi_load_from_memory(data, (int)size, &img_w, &img_h, &channels, 3);
  if (!pixels)
    return false;

  if (img_w <= 0 || img_h <= 0 || img_w > kMobiCoverMaxDimension ||
      img_h > kMobiCoverMaxDimension) {
    stbi_image_free(pixels);
    return false;
  }

  const int thumb_w = 85;
  const int thumb_h = 115;
  float scale_x = (float)img_w / thumb_w;
  float scale_y = (float)img_h / thumb_h;
  float scale = (scale_x > scale_y) ? scale_x : scale_y;
  if (scale < 1.0f)
    scale = 1.0f;

  int final_w = (int)(img_w / scale);
  int final_h = (int)(img_h / scale);
  if (final_w > thumb_w)
    final_w = thumb_w;
  if (final_h > thumb_h)
    final_h = thumb_h;
  if (final_w < 1)
    final_w = 1;
  if (final_h < 1)
    final_h = 1;

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }

  book->coverPixels = new u16[final_w * final_h];
  if (!book->coverPixels) {
    stbi_image_free(pixels);
    return false;
  }
  book->coverWidth = final_w;
  book->coverHeight = final_h;

  for (int y = 0; y < final_h; y++) {
    int src_y = (int)(y * scale);
    if (src_y >= img_h)
      src_y = img_h - 1;
    for (int x = 0; x < final_w; x++) {
      int src_x = (int)(x * scale);
      if (src_x >= img_w)
        src_x = img_w - 1;
      unsigned char *px = &pixels[(src_y * img_w + src_x) * 3];
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * final_w + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return true;
}

static float ScoreCoverCandidate(const MobiCoverCandidate &c) {
  if (c.width <= 0 || c.height <= 0)
    return -100000.0f;

  const float ratio = (float)c.width / (float)c.height;
  float ratio_score = 120.0f - fabsf(ratio - 0.67f) * 260.0f;
  if (ratio_score < -120.0f)
    ratio_score = -120.0f;
  if (ratio_score > 120.0f)
    ratio_score = 120.0f;

  const float orientation_score = (c.height >= c.width) ? 48.0f : -72.0f;

  const float area = (float)c.width * (float)c.height;
  float area_score = area / 5200.0f;
  if (area_score > 96.0f)
    area_score = 96.0f;

  float tiny_penalty = 0.0f;
  if (c.width < 120 || c.height < 160)
    tiny_penalty -= 80.0f;

  return ratio_score + orientation_score + area_score + tiny_penalty;
}

static bool ProbeRecordAsCoverCandidate(const std::string &raw,
                                        const std::vector<u32> &offsets,
                                        u32 record_idx,
                                        MobiCoverCandidate *out) {
  if (!out)
    return false;
  if (record_idx >= offsets.size() - 1)
    return false;

  u32 start = offsets[(size_t)record_idx];
  u32 end = offsets[(size_t)record_idx + 1];
  if (end <= start)
    return false;

  size_t len = (size_t)(end - start);
  if (len < 32 || len > kMobiCoverRecordMaxBytes)
    return false;

  const u8 *data = (const u8 *)raw.data() + start;
  size_t image_off = FindImageStartOffset(data, len);
  if (image_off == SIZE_MAX)
    return false;

  const u8 *image = data + image_off;
  size_t image_len = len - image_off;
  if (image_len == 0 || image_len > (size_t)INT_MAX)
    return false;

  int info_w = 0, info_h = 0, info_c = 0;
  if (stbi_info_from_memory(image, (int)image_len, &info_w, &info_h, &info_c) == 0)
    return false;
  if (info_w <= 0 || info_h <= 0 || info_w > kMobiCoverMaxDimension ||
      info_h > kMobiCoverMaxDimension)
    return false;

  out->record_idx = record_idx;
  out->image_off = image_off;
  out->image_len = image_len;
  out->width = info_w;
  out->height = info_h;
  out->score = ScoreCoverCandidate(*out);
  return true;
}

static u32 FindFirstImageRecordIndex(const std::string &raw,
                                     const std::vector<u32> &offsets,
                                     u32 start_idx,
                                     u32 max_probe_records) {
  const u32 rec_count = (u32)offsets.size() - 1;
  if (rec_count < 2 || start_idx == 0 || start_idx >= rec_count)
    return 0;

  u32 end_idx = rec_count;
  if (max_probe_records > 0 && start_idx + max_probe_records < end_idx)
    end_idx = start_idx + max_probe_records;

  for (u32 i = start_idx; i < end_idx; i++) {
    u32 start = offsets[(size_t)i];
    u32 end = offsets[(size_t)i + 1];
    if (end <= start || end > raw.size())
      continue;

    size_t len = (size_t)(end - start);
    if (len < 32 || len > kMobiCoverRecordMaxBytes)
      continue;

    const u8 *data = (const u8 *)raw.data() + start;
    size_t image_off = FindImageStartOffset(data, len);
    if (image_off == SIZE_MAX)
      continue;

    const u8 *image = data + image_off;
    size_t image_len = len - image_off;
    if (image_len == 0 || image_len > (size_t)INT_MAX)
      continue;

    int info_w = 0, info_h = 0, info_c = 0;
    if (stbi_info_from_memory(image, (int)image_len, &info_w, &info_h,
                              &info_c) == 0)
      continue;
    if (info_w < 48 || info_h < 48 || info_w > kMobiCoverMaxDimension ||
        info_h > kMobiCoverMaxDimension)
      continue;

    return i;
  }

  return 0;
}

static bool TryDecodeCoverCandidate(Book *book, const std::string &raw,
                                    const std::vector<u32> &offsets,
                                    const MobiCoverCandidate &cand, App *app) {
  if (!book || cand.record_idx >= offsets.size() - 1)
    return false;
  u32 start = offsets[(size_t)cand.record_idx];
  u32 end = offsets[(size_t)cand.record_idx + 1];
  if (end <= start || end > raw.size())
    return false;
  const u8 *data = (const u8 *)raw.data() + start;
  if (cand.image_off >= (size_t)(end - start))
    return false;
  const u8 *image = data + cand.image_off;
  size_t image_len = (size_t)(end - start) - cand.image_off;
  if (!DecodeAndScaleToCover(book, image, image_len))
    return false;
  if (app) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: cover record=%u bytes=%u off=%u dim=%dx%d score=%d",
             (unsigned)cand.record_idx, (unsigned)image_len,
             (unsigned)cand.image_off, cand.width, cand.height,
             (int)cand.score);
    app->PrintStatus(msg);
  }
  return true;
}

} // namespace

int mobi_extract_cover(Book *book, const std::string &mobipath) {
  if (!book || mobipath.empty())
    return 1;

  App *app = book->GetApp();
  if (app)
    app->PrintStatus("MOBI: cover scan begin");

  std::string meta_cache_path;
  if (BuildMobiCoverMetaCachePath(mobipath, &meta_cache_path)) {
    mobi_cover_meta_cache::CoverMeta cached_meta;
    if (mobi_cover_meta_cache::Load(meta_cache_path, &cached_meta)) {
      if (cached_meta.kind == mobi_cover_meta_cache::kNoCover) {
        if (app)
          app->PrintStatus("MOBI: cover meta cache none");
        return 5;
      }
      if (TryDecodeCachedCoverMeta(book, mobipath, cached_meta, app))
        return 0;
      remove(meta_cache_path.c_str());
    }
  }

  std::string raw;
  // Cover extraction reads the same MOBI while the library is busy, so reuse
  // the short-read-safe helper instead of stopping on the first partial chunk.
  if (!file_read_utils::ReadPathToStringLimited(mobipath, &raw,
                                                kMobiCoverFileMaxBytes))
    return 2;
  if (raw.empty())
    return BOOK_ERR_CORRUPT;

  std::vector<u32> offsets;
  if (!ParseMobiOffsets(raw, &offsets) || offsets.size() < 3)
    return BOOK_ERR_CORRUPT;

  const u32 rec_count = (u32)offsets.size() - 1;
  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = offsets[0];
  const u32 rec0_end = offsets[1];
  if (rec0_end <= rec0_start || rec0_end > raw.size())
    return 4;

  const u8 *rec0 = data + rec0_start;
  const size_t rec0_len = (size_t)(rec0_end - rec0_start);

  u32 text_rec_count = 0;
  u32 first_non_book_index = 0;
  u32 first_image_index = 0;
  u32 cover_offset = UINT_MAX;
  u32 thumb_offset = UINT_MAX;
  u32 kf8_boundary = UINT_MAX;
  ParseMobiCoverHints(rec0, rec0_len, &text_rec_count, &first_non_book_index,
                      &first_image_index, &cover_offset, &thumb_offset,
                      &kf8_boundary);

  if (text_rec_count >= rec_count)
    text_rec_count = rec_count - 1;

  // Some MOBI files leave first_image at 0 even when EXTH cover offsets are
  // present. In that case, use the first non-text record as inferred base.
  u32 inferred_image_base = first_image_index;
  if (inferred_image_base == 0 && text_rec_count + 1 < rec_count)
    inferred_image_base = text_rec_count + 1;

  // First actual image record after text records. Many real-world MOBIs
  // require this to interpret EXTH cover/thumb offsets correctly.
  u32 detected_first_image_index = 0;
  if (text_rec_count + 1 < rec_count) {
    const u32 start_idx = text_rec_count + 1;
    const u32 first_image_probe_budget =
        mobi_record_scan::FirstImageProbeLimit(rec_count - start_idx);
    detected_first_image_index =
        FindFirstImageRecordIndex(raw, offsets, start_idx,
                                  first_image_probe_budget);
  }
  if (first_image_index == 0 && detected_first_image_index > 0)
    inferred_image_base = detected_first_image_index;

  if (app) {
    char hint_msg[288];
    snprintf(hint_msg, sizeof(hint_msg),
             "MOBI: cover hints primary text=%u first_non_book=%u first_image=%u inferred_base=%u detected_image=%u cover_off=%d thumb_off=%d kf8=%d",
             (unsigned)text_rec_count, (unsigned)first_non_book_index,
             (unsigned)first_image_index, (unsigned)inferred_image_base,
             (unsigned)detected_first_image_index,
             (cover_offset == UINT_MAX) ? -1 : (int)cover_offset,
             (thumb_offset == UINT_MAX) ? -1 : (int)thumb_offset,
             (kf8_boundary == UINT_MAX) ? -1 : (int)kf8_boundary);
    app->PrintStatus(hint_msg);
  }

  // If this is a combo MOBI/KF8, parse hints from KF8 boundary header too.
  u32 kf8_text_rec_count = 0;
  u32 kf8_first_non_book = 0;
  u32 kf8_first_image = 0;
  u32 kf8_cover_offset = UINT_MAX;
  u32 kf8_thumb_offset = UINT_MAX;
  u32 kf8_nested_boundary = UINT_MAX;
  bool has_kf8_hints = false;
  if (kf8_boundary != UINT_MAX && kf8_boundary > 0 && kf8_boundary < rec_count) {
    const u32 rec_start = offsets[(size_t)kf8_boundary];
    const u32 rec_end = offsets[(size_t)kf8_boundary + 1];
    if (rec_end > rec_start && rec_end <= raw.size()) {
      const u8 *kf8_rec = data + rec_start;
      const size_t kf8_len = (size_t)(rec_end - rec_start);
      has_kf8_hints = ParseMobiCoverHints(
          kf8_rec, kf8_len, &kf8_text_rec_count, &kf8_first_non_book,
          &kf8_first_image, &kf8_cover_offset, &kf8_thumb_offset,
          &kf8_nested_boundary);
      if (has_kf8_hints && app) {
        u32 kf8_inferred = kf8_first_image;
        if (kf8_inferred == 0 && kf8_text_rec_count + 1 < rec_count)
          kf8_inferred = kf8_text_rec_count + 1;
        char kf8_msg[256];
        snprintf(kf8_msg, sizeof(kf8_msg),
                 "MOBI: cover hints kf8 rec=%u text=%u first_image=%u inferred_base=%u cover_off=%d thumb_off=%d",
                 (unsigned)kf8_boundary, (unsigned)kf8_text_rec_count,
                 (unsigned)kf8_first_image, (unsigned)kf8_inferred,
                 (kf8_cover_offset == UINT_MAX) ? -1 : (int)kf8_cover_offset,
                 (kf8_thumb_offset == UINT_MAX) ? -1 : (int)kf8_thumb_offset);
        app->PrintStatus(kf8_msg);
      }
    }
  }

  std::vector<u32> candidates;
  std::set<u32> seen;
  std::unordered_map<u32, float> cover_hint_bonus;
  std::unordered_map<u32, float> thumb_hint_bonus;

  auto add_meta_bonus = [&](std::unordered_map<u32, float> *dest, u64 idx,
                            float bonus) {
    if (!dest || idx == 0 || idx >= rec_count || idx > (u64)UINT_MAX)
      return;
    const u32 rec_idx = (u32)idx;
    std::unordered_map<u32, float>::iterator it = dest->find(rec_idx);
    if (it == dest->end() || bonus > it->second)
      (*dest)[rec_idx] = bonus;
    AddRecordCandidate(&candidates, &seen, rec_idx, rec_count);
  };

  auto add_cover_formula_set = [&](u32 base, u32 offset, float strong_bonus,
                                   float weak_bonus) {
    if (offset == UINT_MAX)
      return;
    if (base > 0) {
      add_meta_bonus(&cover_hint_bonus, (u64)base + (u64)offset, strong_bonus);
      add_meta_bonus(&cover_hint_bonus, (u64)base + (u64)offset + 1,
                     strong_bonus - 30.0f);
      if (offset > 0)
        add_meta_bonus(&cover_hint_bonus, (u64)base + (u64)(offset - 1),
                       strong_bonus - 35.0f);
    }
    add_meta_bonus(&cover_hint_bonus, (u64)offset, weak_bonus);
    add_meta_bonus(&cover_hint_bonus, (u64)offset + 1, weak_bonus - 20.0f);
  };

  auto add_thumb_formula_set = [&](u32 base, u32 offset, float strong_bonus,
                                   float weak_bonus) {
    if (offset == UINT_MAX)
      return;
    if (base > 0) {
      add_meta_bonus(&thumb_hint_bonus, (u64)base + (u64)offset, strong_bonus);
      add_meta_bonus(&thumb_hint_bonus, (u64)base + (u64)offset + 1,
                     strong_bonus - 30.0f);
      if (offset > 0)
        add_meta_bonus(&thumb_hint_bonus, (u64)base + (u64)(offset - 1),
                       strong_bonus - 35.0f);
    }
    add_meta_bonus(&thumb_hint_bonus, (u64)offset, weak_bonus);
    add_meta_bonus(&thumb_hint_bonus, (u64)offset + 1, weak_bonus - 20.0f);
  };

  if (cover_offset != UINT_MAX) {
    // Kindle metadata usually stores cover as offset from first image record,
    // but malformed books can store it as absolute or 1-based. Probe all
    // common variants before any heuristic global scan.
    if (detected_first_image_index > 0)
      add_cover_formula_set(detected_first_image_index, cover_offset, 520.0f,
                            365.0f);
    add_cover_formula_set(inferred_image_base, cover_offset, 430.0f, 300.0f);
    if (first_image_index > 0)
      add_cover_formula_set(first_image_index, cover_offset, 410.0f, 280.0f);
    if (first_non_book_index > 0)
      add_cover_formula_set(first_non_book_index, cover_offset, 360.0f, 250.0f);
  }

  if (thumb_offset != UINT_MAX) {
    if (detected_first_image_index > 0)
      add_thumb_formula_set(detected_first_image_index, thumb_offset, 305.0f,
                            215.0f);
    add_thumb_formula_set(inferred_image_base, thumb_offset, 250.0f, 170.0f);
    if (first_image_index > 0)
      add_thumb_formula_set(first_image_index, thumb_offset, 235.0f, 160.0f);
    if (first_non_book_index > 0)
      add_thumb_formula_set(first_non_book_index, thumb_offset, 210.0f, 145.0f);
  }

  if (has_kf8_hints) {
    u32 kf8_inferred_base = kf8_first_image;
    if (kf8_inferred_base == 0 && kf8_text_rec_count + 1 < rec_count)
      kf8_inferred_base = kf8_text_rec_count + 1;
    if (kf8_cover_offset != UINT_MAX) {
      add_cover_formula_set(kf8_inferred_base, kf8_cover_offset, 470.0f, 335.0f);
      if (kf8_first_image > 0)
        add_cover_formula_set(kf8_first_image, kf8_cover_offset, 450.0f, 320.0f);
      if (kf8_first_non_book > 0)
        add_cover_formula_set(kf8_first_non_book, kf8_cover_offset, 390.0f, 280.0f);
    }
    if (kf8_thumb_offset != UINT_MAX) {
      add_thumb_formula_set(kf8_inferred_base, kf8_thumb_offset, 285.0f, 200.0f);
      if (kf8_first_image > 0)
        add_thumb_formula_set(kf8_first_image, kf8_thumb_offset, 270.0f, 185.0f);
      if (kf8_first_non_book > 0)
        add_thumb_formula_set(kf8_first_non_book, kf8_thumb_offset, 230.0f, 165.0f);
    }
  }

  if (first_image_index > 0)
    AddRecordCandidate(&candidates, &seen, first_image_index, rec_count);
  if (first_non_book_index > 0)
    AddRecordCandidate(&candidates, &seen, first_non_book_index, rec_count);

  u32 start = (text_rec_count + 1 < rec_count) ? text_rec_count + 1 : 1;
  u32 end_probe = start + 96;
  if (end_probe > rec_count)
    end_probe = rec_count;
  for (u32 i = start; i < end_probe; i++)
    AddRecordCandidate(&candidates, &seen, i, rec_count);

  // First pass: trust EXTH metadata candidates before heuristics, but still
  // rank them to avoid taking the first decodable (often wrong) record.
  std::vector<MobiCoverCandidate> meta_ranked;
  meta_ranked.reserve(cover_hint_bonus.size() + thumb_hint_bonus.size());
  for (std::unordered_map<u32, float>::const_iterator it =
           cover_hint_bonus.begin();
       it != cover_hint_bonus.end(); ++it) {
    MobiCoverCandidate c;
    if (!ProbeRecordAsCoverCandidate(raw, offsets, it->first, &c))
      continue;
    if (c.width < 24 || c.height < 24)
      continue;
    c.score += it->second;
    meta_ranked.push_back(c);
  }
  for (std::unordered_map<u32, float>::const_iterator it =
           thumb_hint_bonus.begin();
       it != thumb_hint_bonus.end(); ++it) {
    MobiCoverCandidate c;
    if (!ProbeRecordAsCoverCandidate(raw, offsets, it->first, &c))
      continue;
    if (c.width < 24 || c.height < 24)
      continue;
    c.score += it->second;
    meta_ranked.push_back(c);
  }

  if (!meta_ranked.empty()) {
    std::sort(meta_ranked.begin(), meta_ranked.end(),
              [](const MobiCoverCandidate &a, const MobiCoverCandidate &b) {
                if (a.score == b.score)
                  return a.record_idx < b.record_idx;
                return a.score > b.score;
              });
    if (app) {
      const size_t preview = std::min((size_t)5, meta_ranked.size());
      for (size_t i = 0; i < preview; i++) {
        char meta_msg[224];
        snprintf(meta_msg, sizeof(meta_msg),
                 "MOBI: cover meta cand[%u] rec=%u dim=%dx%d score=%d",
                 (unsigned)i, (unsigned)meta_ranked[i].record_idx,
                 meta_ranked[i].width, meta_ranked[i].height,
                 (int)meta_ranked[i].score);
        app->PrintStatus(meta_msg);
      }
    }
    for (size_t i = 0; i < meta_ranked.size(); i++) {
      if (TryDecodeCoverCandidate(book, raw, offsets, meta_ranked[i], app)) {
        SaveMobiCoverMetaCandidate(meta_cache_path, offsets, meta_ranked[i]);
        return 0;
      }
    }
  }

  std::vector<MobiCoverCandidate> ranked;
  ranked.reserve(candidates.size() + 64);
  for (size_t i = 0; i < candidates.size(); i++) {
    MobiCoverCandidate c;
    if (!ProbeRecordAsCoverCandidate(raw, offsets, candidates[i], &c))
      continue;
    int bonus = 220 - (int)i * 12;
    if (bonus < 0)
      bonus = 0;
    c.score += (float)bonus;
    ranked.push_back(c);
  }

  // Last resort: scan remaining non-text records.
  const u32 last_resort_end =
      std::min<u32>(rec_count, start + mobi_record_scan::CoverLastResortProbeLimit(
                                           rec_count - start));
  for (u32 i = start; i < last_resort_end; i++) {
    if (seen.find(i) != seen.end())
      continue;
    MobiCoverCandidate c;
    if (!ProbeRecordAsCoverCandidate(raw, offsets, i, &c))
      continue;
    c.score -= 24.0f; // Prefer explicit/proximal candidates over global scan.
    ranked.push_back(c);
  }

  if (!ranked.empty()) {
    std::sort(ranked.begin(), ranked.end(),
              [](const MobiCoverCandidate &a, const MobiCoverCandidate &b) {
                if (a.score == b.score)
                  return a.record_idx < b.record_idx;
                return a.score > b.score;
              });
    for (size_t i = 0; i < ranked.size(); i++) {
      if (TryDecodeCoverCandidate(book, raw, offsets, ranked[i], app)) {
        SaveMobiCoverMetaCandidate(meta_cache_path, offsets, ranked[i]);
        return 0;
      }
    }
  }

  SaveMobiCoverMetaMiss(meta_cache_path);
  if (app)
    app->PrintStatus("MOBI: cover scan none");
  return 5;
}

bool mobi_parse_offsets(const std::string &raw, std::vector<u32> *offsets) {
  return ParseMobiOffsets(raw, offsets);
}

size_t mobi_find_image_start_offset(const u8 *data, size_t len) {
  return FindImageStartOffset(data, len);
}

static bool ParseMobiImageRefToken(const std::string &token, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;
  if (token.empty())
    return false;

  unsigned long parsed = 0;
  bool all_digits = true;
  for (size_t i = 0; i < token.size(); i++) {
    unsigned char c = (unsigned char)token[i];
    if (!isdigit(c)) {
      all_digits = false;
      break;
    }
    parsed = parsed * 10UL + (unsigned long)(c - '0');
    if (parsed > 65535UL)
      return false;
  }
  if (all_digits) {
    if (parsed == 0 || parsed > 65535UL)
      return false;
    if (recindex_out)
      *recindex_out = (u16)parsed;
    return true;
  }

  parsed = 0;
  for (size_t i = 0; i < token.size(); i++) {
    unsigned char c = (unsigned char)token[i];
    unsigned value = 0;
    if (c >= '0' && c <= '9') {
      value = (unsigned)(c - '0');
    } else if (c >= 'A' && c <= 'V') {
      value = 10u + (unsigned)(c - 'A');
    } else if (c >= 'a' && c <= 'v') {
      value = 10u + (unsigned)(c - 'a');
    } else {
      return false;
    }
    parsed = parsed * 32UL + (unsigned long)value;
    if (parsed > 65535UL)
      return false;
  }
  if (parsed == 0 || parsed > 65535UL)
    return false;
  if (recindex_out)
    *recindex_out = (u16)parsed;
  return true;
}

static bool ParseMobiImageRefValue(const std::string &value, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;
  const std::string trimmed = Trim(value);
  if (trimmed.empty())
    return false;

  if (ParseMobiImageRefToken(trimmed, recindex_out))
    return true;

  std::string lower = trimmed;
  for (size_t i = 0; i < lower.size(); i++)
    lower[i] = (char)tolower((unsigned char)lower[i]);
  static const char *kPrefixes[] = {"kindle:embed:", "kindle:flow:", "embed:"};
  for (size_t p = 0; p < sizeof(kPrefixes) / sizeof(kPrefixes[0]); p++) {
    const std::string prefix = kPrefixes[p];
    if (lower.compare(0, prefix.size(), prefix) != 0)
      continue;
    size_t start = prefix.size();
    size_t end = start;
    while (end < trimmed.size()) {
      unsigned char c = (unsigned char)trimmed[end];
      if (isalnum(c) == 0)
        break;
      end++;
    }
    if (end > start)
      return ParseMobiImageRefToken(trimmed.substr(start, end - start),
                                    recindex_out);
  }

  return false;
}

bool mobi_extract_image_recindex(const std::string &tag, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;

  static const char *kAttrs[] = {"recindex", "lowrecindex", "hirecindex",
                                 "src", "href"};
  for (size_t i = 0; i < sizeof(kAttrs) / sizeof(kAttrs[0]); i++) {
    std::string value = ExtractHtmlAttrValue(tag, kAttrs[i]);
    if (value.empty())
      continue;
    if (ParseMobiImageRefValue(value, recindex_out))
      return true;
  }
  return false;
}

std::string mobi_inline_image_path(u16 recindex) {
  char out[32];
  snprintf(out, sizeof(out), "mobi:recindex:%u", (unsigned)recindex);
  return std::string(out);
}

bool mobi_parse_inline_image_path(const std::string &path, u16 *recindex_out) {
  if (recindex_out)
    *recindex_out = 0;

  static const char *kPrefix = "mobi:recindex:";
  if (path.compare(0, strlen(kPrefix), kPrefix) != 0)
    return false;

  const std::string value = path.substr(strlen(kPrefix));
  if (value.empty())
    return false;

  unsigned long parsed = 0;
  for (size_t i = 0; i < value.size(); i++) {
    unsigned char c = (unsigned char)value[i];
    if (!isdigit(c))
      return false;
    parsed = parsed * 10UL + (unsigned long)(c - '0');
    if (parsed == 0 || parsed > 65535UL)
      return false;
  }

  if (parsed == 0 || parsed > 65535UL)
    return false;
  if (recindex_out)
    *recindex_out = (u16)parsed;
  return true;
}
