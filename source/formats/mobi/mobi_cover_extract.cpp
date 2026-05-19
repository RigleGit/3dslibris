/*
    3dslibris - mobi_cover_extract.cpp
    Extracted from mobi.cpp. Holds the MOBI/AZW cover-decode pipeline:
    EXTH cover hints, image-record scan, format detection, and decode
    + scale to an RGB565 thumbnail for the browser cover cache.

    Inline-image path parsing (mobi:recindex:N) stays in mobi.cpp.
    The mobi_parse_offsets and mobi_find_image_start_offset symbols are
    declared in mobi.h and exported from this translation unit; both wrap
    helpers used internally by the cover scan and by book_inline_image.

    No behavior change — pure code motion.
*/

#include "formats/mobi/mobi.h"

#include "book/cover_layout_constants.h"
#include "formats/common/book_error.h"
#include "formats/mobi/mobi_cover_meta_cache.h"
#include "formats/mobi/mobi_record_scan.h"
#include "formats/mobi/mobi_cover_utils.h"
#include "shared/aspect_fit_utils.h"
#include "shared/cover_decode_utils.h"
#include "shared/path_constants.h"
#include "shared/status_reporter.h"
#include "stb_image.h"
#include "shared/string_utils.h"

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

static const size_t kMobiCoverRecordMaxBytes = 8 * 1024 * 1024;
static const int kMobiCoverMaxDimension = 2048;
static const std::string &kMobiCoverMetaCacheBaseDir = paths::GetCacheBaseDir();
static const std::string &kMobiCoverMetaCacheDir = paths::GetMobiCoverMetaCacheDir();

struct MobiCoverCandidate {
  u32 record_idx;
  size_t image_off;
  size_t image_len;
  int width;
  int height;
  float score;
};

static bool DecodeAndScaleToCover(Book *book, const u8 *data, size_t size);

class MobiRecordReader {
public:
  explicit MobiRecordReader(const std::string &path)
      : path_(path), fp_(NULL), file_size_(0) {}

  ~MobiRecordReader() {
    if (fp_)
      fclose(fp_);
  }

  bool EnsureOpen() {
    if (fp_)
      return true;
    if (path_.empty())
      return false;
    fp_ = fopen(path_.c_str(), "rb");
    if (!fp_)
      return false;
    if (fseek(fp_, 0L, SEEK_END) != 0) {
      fclose(fp_);
      fp_ = NULL;
      return false;
    }
    long end = ftell(fp_);
    if (end < 0) {
      fclose(fp_);
      fp_ = NULL;
      return false;
    }
    file_size_ = (size_t)end;
    rewind(fp_);
    return true;
  }

  size_t file_size() const { return file_size_; }

  bool ReadPrefix(size_t len, std::vector<u8> *out) {
    if (!out || len == 0 || !EnsureOpen() || len > file_size_)
      return false;
    out->resize(len);
    rewind(fp_);
    if (fread(out->data(), 1, len, fp_) != len) {
      out->clear();
      return false;
    }
    return true;
  }

  bool ReadRange(u32 start, u32 end, std::vector<u8> *out) {
    if (!out || end <= start || !EnsureOpen())
      return false;
    const size_t len = (size_t)(end - start);
    if (len == 0 || len > kMobiCoverRecordMaxBytes || end > file_size_)
      return false;
    out->resize(len);
    if (fseek(fp_, (long)start, SEEK_SET) != 0 ||
        fread(out->data(), 1, len, fp_) != len) {
      out->clear();
      return false;
    }
    return true;
  }

  bool GetRecord(const std::vector<u32> &offsets, u32 record_idx,
                 const std::vector<u8> **out) {
    if (!out || record_idx + 1 >= offsets.size())
      return false;
    std::unordered_map<u32, std::vector<u8>>::const_iterator it =
        record_cache_.find(record_idx);
    if (it != record_cache_.end()) {
      *out = &it->second;
      return true;
    }

    std::vector<u8> record;
    if (!ReadRange(offsets[(size_t)record_idx], offsets[(size_t)record_idx + 1],
                   &record)) {
      return false;
    }

    std::pair<std::unordered_map<u32, std::vector<u8>>::iterator, bool> inserted =
        record_cache_.insert(
            std::make_pair(record_idx, std::vector<u8>()));
    inserted.first->second.swap(record);
    *out = &inserted.first->second;
    return true;
  }

private:
  std::string path_;
  FILE *fp_;
  size_t file_size_;
  std::unordered_map<u32, std::vector<u8>> record_cache_;
};

static void EnsureMobiCoverMetaCacheDirs() {
  static bool initialized = false;
  if (initialized)
    return;
  mkdir(kMobiCoverMetaCacheBaseDir.c_str(), 0777);
  mkdir(kMobiCoverMetaCacheDir.c_str(), 0777);
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
                                     IStatusReporter *reporter) {
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

  if (reporter) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: cover meta cache hit rec=%u off=%u dim=%ux%u",
             (unsigned)meta.record_index, (unsigned)meta.image_offset,
             (unsigned)meta.width, (unsigned)meta.height);
    reporter->PrintStatus(msg);
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

static bool ParseMobiOffsets(const std::string &raw, std::vector<u32> *offsets) {
  if (!offsets || raw.size() < 78 || raw.size() > (size_t)UINT_MAX)
    return false;
  return mobi_cover_utils::ParsePdbRecordOffsets(
      (const uint8_t *)raw.data(), raw.size(), (uint32_t)raw.size(), offsets);
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
    *text_rec_count = mobi_cover_utils::ReadBE16(rec0 + 8);
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
  const u32 mobi_len = mobi_cover_utils::ReadBE32(mobi + 4);
  if (mobi_len < 0x20 || rec0_len < 16 + (size_t)mobi_len)
    return false;

  if (mobi_len >= 0x84 && rec0_len >= 16 + (size_t)0x84 && first_non_book_index)
    *first_non_book_index = mobi_cover_utils::ReadBE32(mobi + 0x80);
  if (mobi_len >= 0x70 && rec0_len >= 16 + (size_t)0x70 && first_image_index)
    *first_image_index = mobi_cover_utils::ReadBE32(mobi + 0x6C);

  // EXTH records (when bit 0x40 is set in EXTH flags at 0x80)
  if (mobi_len >= 0x84) {
    u32 exth_flags = mobi_cover_utils::ReadBE32(mobi + 0x80);
    const bool flagged_exth = (exth_flags & 0x40) != 0;
    const u8 *exth = mobi + mobi_len;
    size_t remain = rec0_len - (size_t)(exth - rec0);
    const bool looks_like_exth =
        (remain >= 12 && memcmp(exth, "EXTH", 4) == 0);
    if (flagged_exth || looks_like_exth) {
      if (looks_like_exth) {
        u32 exth_len = mobi_cover_utils::ReadBE32(exth + 4);
        u32 recs = mobi_cover_utils::ReadBE32(exth + 8);
        if (exth_len >= 12 && exth_len <= remain) {
          const u8 *p = exth + 12;
          const u8 *end = exth + exth_len;
          for (u32 i = 0; i < recs && p + 8 <= end; i++) {
            u32 type = mobi_cover_utils::ReadBE32(p + 0);
            u32 size = mobi_cover_utils::ReadBE32(p + 4);
            if (size < 8 || p + size > end)
              break;
            if ((type == 201 || type == 202) && size >= 12) {
              u32 val = mobi_cover_utils::ReadBE32(p + 8);
              if (type == 201 && cover_offset)
                *cover_offset = val;
              if (type == 202 && thumb_offset)
                *thumb_offset = val;
            } else if (type == 121 && size >= 12) {
              // EXTH 121 = KF8 boundary record index (combo files).
              if (kf8_boundary)
                *kf8_boundary = mobi_cover_utils::ReadBE32(p + 8);
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
  return cover_decode_utils::DecodeImageToCoverThumb(book, data, size,
                                                     kMobiCoverMaxDimension);
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

static bool ProbeRecordAsCoverCandidate(MobiRecordReader *reader,
                                        const std::vector<u32> &offsets,
                                        u32 record_idx,
                                        MobiCoverCandidate *out) {
  if (!reader || !out)
    return false;
  if (record_idx >= offsets.size() - 1)
    return false;

  const std::vector<u8> *record = NULL;
  if (!reader->GetRecord(offsets, record_idx, &record) || !record)
    return false;

  const size_t len = record->size();
  if (len < 32 || len > kMobiCoverRecordMaxBytes)
    return false;

  const u8 *data = record->data();
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

static u32 FindFirstImageRecordIndex(MobiRecordReader *reader,
                                     const std::vector<u32> &offsets,
                                     u32 start_idx,
                                     u32 max_probe_records) {
  if (!reader)
    return 0;
  const u32 rec_count = (u32)offsets.size() - 1;
  if (rec_count < 2 || start_idx == 0 || start_idx >= rec_count)
    return 0;

  u32 end_idx = rec_count;
  if (max_probe_records > 0 && start_idx + max_probe_records < end_idx)
    end_idx = start_idx + max_probe_records;

  for (u32 i = start_idx; i < end_idx; i++) {
    const std::vector<u8> *record = NULL;
    if (!reader->GetRecord(offsets, i, &record) || !record)
      continue;

    const size_t len = record->size();
    if (len < 32 || len > kMobiCoverRecordMaxBytes)
      continue;

    const u8 *data = record->data();
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

// Bag of MOBI cover hints derived from the primary record-0 MOBI header
// (and from a KF8-boundary record header if the file is a combo MOBI/KF8).
// All offsets are PDB record indices; UINT_MAX means "absent". Used by
// BuildCoverHintScoringMaps to seed the cover/thumb score maps.
struct MobiCoverHints {
  u32 rec_count = 0;
  u32 text_rec_count = 0;
  u32 first_non_book_index = 0;
  u32 first_image_index = 0;
  u32 inferred_image_base = 0;
  u32 detected_first_image_index = 0;
  u32 cover_offset = UINT_MAX;
  u32 thumb_offset = UINT_MAX;
  u32 kf8_boundary = UINT_MAX;

  bool has_kf8 = false;
  u32 kf8_text_rec_count = 0;
  u32 kf8_first_non_book = 0;
  u32 kf8_first_image = 0;
  u32 kf8_inferred_base = 0;
  u32 kf8_cover_offset = UINT_MAX;
  u32 kf8_thumb_offset = UINT_MAX;
};

// Meta-cache probe: returns 0 on a successful cached decode, 5 on a cached
// "no cover" outcome, and -1 when there is no cached answer (caller should
// fall through to the full scan). On a cache hit that fails to decode, the
// stale cache entry is removed so the next pass writes fresh.
static int TryDecodeCoverFromMetaCache(Book *book, const std::string &mobipath,
                                       const std::string &meta_cache_path,
                                       IStatusReporter *reporter) {
  if (meta_cache_path.empty())
    return -1;
  mobi_cover_meta_cache::CoverMeta cached_meta;
  if (!mobi_cover_meta_cache::Load(meta_cache_path, &cached_meta))
    return -1;
  if (cached_meta.kind == mobi_cover_meta_cache::kNoCover) {
    if (reporter)
      reporter->PrintStatus("MOBI: cover meta cache none");
    return 5;
  }
  if (TryDecodeCachedCoverMeta(book, mobipath, cached_meta, reporter))
    return 0;
  remove(meta_cache_path.c_str());
  return -1;
}

// Read rec0 and the optional KF8 boundary record, parse cover hints from
// both, log them, and fill `out`. Returns false on a malformed record-0;
// the caller should treat that as a corrupt MOBI.
static bool ParseAllMobiCoverHints(MobiRecordReader *reader,
                                   const std::vector<u32> &offsets,
                                   u32 rec_count, IStatusReporter *reporter,
                                   MobiCoverHints *out) {
  if (!reader || !out)
    return false;
  out->rec_count = rec_count;
  const std::vector<u8> *rec0_buf = NULL;
  if (!reader->GetRecord(offsets, 0, &rec0_buf) || !rec0_buf ||
      rec0_buf->empty())
    return false;
  const u8 *rec0 = rec0_buf->data();
  const size_t rec0_len = rec0_buf->size();
  ParseMobiCoverHints(rec0, rec0_len, &out->text_rec_count,
                      &out->first_non_book_index, &out->first_image_index,
                      &out->cover_offset, &out->thumb_offset,
                      &out->kf8_boundary);

  if (out->text_rec_count >= rec_count)
    out->text_rec_count = rec_count - 1;

  // Some MOBI files leave first_image at 0 even when EXTH cover offsets are
  // present. Fall back to "first non-text record" as the inferred base.
  out->inferred_image_base = out->first_image_index;
  if (out->inferred_image_base == 0 && out->text_rec_count + 1 < rec_count)
    out->inferred_image_base = out->text_rec_count + 1;

  // Probe forward to find the first record that decodes as an image. Many
  // real-world MOBIs need this for the EXTH offsets to land correctly.
  if (out->text_rec_count + 1 < rec_count) {
    const u32 start_idx = out->text_rec_count + 1;
    const u32 budget =
        mobi_record_scan::FirstImageProbeLimit(rec_count - start_idx);
    out->detected_first_image_index =
        FindFirstImageRecordIndex(reader, offsets, start_idx, budget);
  }
  if (out->first_image_index == 0 && out->detected_first_image_index > 0)
    out->inferred_image_base = out->detected_first_image_index;

  if (reporter) {
    char msg[288];
    snprintf(msg, sizeof(msg),
             "MOBI: cover hints primary text=%u first_non_book=%u first_image=%u inferred_base=%u detected_image=%u cover_off=%d thumb_off=%d kf8=%d",
             (unsigned)out->text_rec_count,
             (unsigned)out->first_non_book_index,
             (unsigned)out->first_image_index,
             (unsigned)out->inferred_image_base,
             (unsigned)out->detected_first_image_index,
             (out->cover_offset == UINT_MAX) ? -1 : (int)out->cover_offset,
             (out->thumb_offset == UINT_MAX) ? -1 : (int)out->thumb_offset,
             (out->kf8_boundary == UINT_MAX) ? -1 : (int)out->kf8_boundary);
    reporter->PrintStatus(msg);
  }

  // KF8 combo files: parse the boundary record's MOBI header too.
  if (out->kf8_boundary != UINT_MAX && out->kf8_boundary > 0 &&
      out->kf8_boundary < rec_count) {
    const u32 rec_start = offsets[(size_t)out->kf8_boundary];
    const u32 rec_end = offsets[(size_t)out->kf8_boundary + 1];
    const std::vector<u8> *kf8_record = NULL;
    if (rec_end > rec_start &&
        reader->GetRecord(offsets, out->kf8_boundary, &kf8_record) &&
        kf8_record) {
      u32 kf8_nested = UINT_MAX;
      out->has_kf8 = ParseMobiCoverHints(
          kf8_record->data(), kf8_record->size(), &out->kf8_text_rec_count,
          &out->kf8_first_non_book, &out->kf8_first_image,
          &out->kf8_cover_offset, &out->kf8_thumb_offset, &kf8_nested);
      if (out->has_kf8) {
        out->kf8_inferred_base = out->kf8_first_image;
        if (out->kf8_inferred_base == 0 &&
            out->kf8_text_rec_count + 1 < rec_count)
          out->kf8_inferred_base = out->kf8_text_rec_count + 1;
        if (reporter) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "MOBI: cover hints kf8 rec=%u text=%u first_image=%u inferred_base=%u cover_off=%d thumb_off=%d",
                   (unsigned)out->kf8_boundary,
                   (unsigned)out->kf8_text_rec_count,
                   (unsigned)out->kf8_first_image,
                   (unsigned)out->kf8_inferred_base,
                   (out->kf8_cover_offset == UINT_MAX)
                       ? -1
                       : (int)out->kf8_cover_offset,
                   (out->kf8_thumb_offset == UINT_MAX)
                       ? -1
                       : (int)out->kf8_thumb_offset);
          reporter->PrintStatus(msg);
        }
      }
    }
  }

  return true;
}

static bool TryDecodeCoverCandidate(Book *book, MobiRecordReader *reader,
                                    const std::vector<u32> &offsets,
                                    const MobiCoverCandidate &cand,
                                    IStatusReporter *reporter) {
  if (!book || !reader || cand.record_idx >= offsets.size() - 1)
    return false;
  const std::vector<u8> *record = NULL;
  if (!reader->GetRecord(offsets, cand.record_idx, &record) || !record)
    return false;
  if (cand.image_off >= record->size())
    return false;
  const u8 *image = record->data() + cand.image_off;
  size_t image_len = record->size() - cand.image_off;
  if (!DecodeAndScaleToCover(book, image, image_len))
    return false;
  if (reporter) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: cover record=%u bytes=%u off=%u dim=%dx%d score=%d",
             (unsigned)cand.record_idx, (unsigned)image_len,
             (unsigned)cand.image_off, cand.width, cand.height,
             (int)cand.score);
    reporter->PrintStatus(msg);
  }
  return true;
}

} // namespace

int mobi_extract_cover(Book *book, const std::string &mobipath) {
  if (!book || mobipath.empty())
    return 1;

  IStatusReporter *reporter = book->GetStatusReporter();
  if (reporter)
    reporter->PrintStatus("MOBI: cover scan begin");

  std::string meta_cache_path;
  BuildMobiCoverMetaCachePath(mobipath, &meta_cache_path);
  const int meta_rc =
      TryDecodeCoverFromMetaCache(book, mobipath, meta_cache_path, reporter);
  if (meta_rc >= 0)
    return meta_rc;

  MobiRecordReader reader(mobipath);
  if (!reader.EnsureOpen())
    return 2;
  if (reader.file_size() == 0 || reader.file_size() > (size_t)UINT_MAX)
    return BOOK_ERR_CORRUPT;

  std::vector<u8> offset_table;
  if (!reader.ReadPrefix(78, &offset_table))
    return BOOK_ERR_CORRUPT;
  const size_t offset_table_size = mobi_cover_utils::PdbOffsetTableSizeFromHeader(
      offset_table.data(), offset_table.size());
  if (offset_table_size == 0 || offset_table_size > reader.file_size() ||
      !reader.ReadPrefix(offset_table_size, &offset_table)) {
    return BOOK_ERR_CORRUPT;
  }

  std::vector<u32> offsets;
  if (!mobi_cover_utils::ParsePdbRecordOffsets(
          offset_table.data(), offset_table.size(),
          (uint32_t)reader.file_size(), &offsets) ||
      offsets.size() < 3) {
    return BOOK_ERR_CORRUPT;
  }

  const u32 rec_count = (u32)offsets.size() - 1;
  MobiCoverHints hints;
  if (!ParseAllMobiCoverHints(&reader, offsets, rec_count, reporter, &hints))
    return 4;

  // Expose hint fields under the names the scoring code below has always
  // used, so the formula-set call sites stay one-to-one with the original
  // implementation.
  const u32 text_rec_count = hints.text_rec_count;
  const u32 first_non_book_index = hints.first_non_book_index;
  const u32 first_image_index = hints.first_image_index;
  const u32 inferred_image_base = hints.inferred_image_base;
  const u32 detected_first_image_index = hints.detected_first_image_index;
  const u32 cover_offset = hints.cover_offset;
  const u32 thumb_offset = hints.thumb_offset;
  const bool has_kf8_hints = hints.has_kf8;
  const u32 kf8_text_rec_count = hints.kf8_text_rec_count;
  const u32 kf8_first_non_book = hints.kf8_first_non_book;
  const u32 kf8_first_image = hints.kf8_first_image;
  const u32 kf8_cover_offset = hints.kf8_cover_offset;
  const u32 kf8_thumb_offset = hints.kf8_thumb_offset;

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

  // Expand a single EXTH offset into bonuses against base+offset (strong) and
  // raw offset (weak) plus ±1 fuzz, all writing to the same hint map. Both the
  // cover and thumb scoring use this; previously two near-identical lambdas.
  auto add_formula_set = [&](std::unordered_map<u32, float> *dest, u32 base,
                             u32 offset, float strong_bonus, float weak_bonus) {
    if (offset == UINT_MAX)
      return;
    if (base > 0) {
      add_meta_bonus(dest, (u64)base + (u64)offset, strong_bonus);
      add_meta_bonus(dest, (u64)base + (u64)offset + 1, strong_bonus - 30.0f);
      if (offset > 0)
        add_meta_bonus(dest, (u64)base + (u64)(offset - 1), strong_bonus - 35.0f);
    }
    add_meta_bonus(dest, (u64)offset, weak_bonus);
    add_meta_bonus(dest, (u64)offset + 1, weak_bonus - 20.0f);
  };

  // Kindle metadata usually stores cover as offset from first image record,
  // but malformed books can store it as absolute or 1-based. Probe each
  // common variant before any heuristic global scan. The "inferred" base is
  // always probed (even when 0, so weak/raw-offset bonus still applies); the
  // other bases require index>0 to avoid double-counting the same weak slot.
  if (cover_offset != UINT_MAX) {
    if (detected_first_image_index > 0)
      add_formula_set(&cover_hint_bonus, detected_first_image_index,
                      cover_offset, 520.0f, 365.0f);
    add_formula_set(&cover_hint_bonus, inferred_image_base, cover_offset,
                    430.0f, 300.0f);
    if (first_image_index > 0)
      add_formula_set(&cover_hint_bonus, first_image_index, cover_offset,
                      410.0f, 280.0f);
    if (first_non_book_index > 0)
      add_formula_set(&cover_hint_bonus, first_non_book_index, cover_offset,
                      360.0f, 250.0f);
  }
  if (thumb_offset != UINT_MAX) {
    if (detected_first_image_index > 0)
      add_formula_set(&thumb_hint_bonus, detected_first_image_index,
                      thumb_offset, 305.0f, 215.0f);
    add_formula_set(&thumb_hint_bonus, inferred_image_base, thumb_offset,
                    250.0f, 170.0f);
    if (first_image_index > 0)
      add_formula_set(&thumb_hint_bonus, first_image_index, thumb_offset,
                      235.0f, 160.0f);
    if (first_non_book_index > 0)
      add_formula_set(&thumb_hint_bonus, first_non_book_index, thumb_offset,
                      210.0f, 145.0f);
  }

  if (has_kf8_hints) {
    u32 kf8_inferred_base = kf8_first_image;
    if (kf8_inferred_base == 0 && kf8_text_rec_count + 1 < rec_count)
      kf8_inferred_base = kf8_text_rec_count + 1;
    if (kf8_cover_offset != UINT_MAX) {
      add_formula_set(&cover_hint_bonus, kf8_inferred_base, kf8_cover_offset,
                      470.0f, 335.0f);
      if (kf8_first_image > 0)
        add_formula_set(&cover_hint_bonus, kf8_first_image, kf8_cover_offset,
                        450.0f, 320.0f);
      if (kf8_first_non_book > 0)
        add_formula_set(&cover_hint_bonus, kf8_first_non_book,
                        kf8_cover_offset, 390.0f, 280.0f);
    }
    if (kf8_thumb_offset != UINT_MAX) {
      add_formula_set(&thumb_hint_bonus, kf8_inferred_base, kf8_thumb_offset,
                      285.0f, 200.0f);
      if (kf8_first_image > 0)
        add_formula_set(&thumb_hint_bonus, kf8_first_image, kf8_thumb_offset,
                        270.0f, 185.0f);
      if (kf8_first_non_book > 0)
        add_formula_set(&thumb_hint_bonus, kf8_first_non_book,
                        kf8_thumb_offset, 230.0f, 165.0f);
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
  auto collect_hint_candidates =
      [&](const std::unordered_map<u32, float> &hint_map) {
        for (const auto &kv : hint_map) {
          MobiCoverCandidate c;
          if (!ProbeRecordAsCoverCandidate(&reader, offsets, kv.first, &c))
            continue;
          if (c.width < 24 || c.height < 24)
            continue;
          c.score += kv.second;
          meta_ranked.push_back(c);
        }
      };
  collect_hint_candidates(cover_hint_bonus);
  collect_hint_candidates(thumb_hint_bonus);

  // Sort highest-score-first (tie-break by record index) and try to decode
  // each in turn; on first success, save the candidate to the meta cache and
  // return. Shared by the meta-priority and heuristic+last-resort passes.
  auto sort_by_score = [](MobiCoverCandidate &a, MobiCoverCandidate &b) {
    if (a.score == b.score)
      return a.record_idx < b.record_idx;
    return a.score > b.score;
  };
  auto try_decode_ranked = [&](std::vector<MobiCoverCandidate> &v) -> bool {
    std::sort(v.begin(), v.end(), sort_by_score);
    for (const MobiCoverCandidate &c : v) {
      if (TryDecodeCoverCandidate(book, &reader, offsets, c, reporter)) {
        SaveMobiCoverMetaCandidate(meta_cache_path, offsets, c);
        return true;
      }
    }
    return false;
  };

  if (!meta_ranked.empty()) {
    if (reporter) {
      // Log the top-5 by score before iterating, so a wrong winner is easy to
      // diagnose from the user's log without rebuilding with a deeper trace.
      std::vector<MobiCoverCandidate> preview_copy = meta_ranked;
      std::sort(preview_copy.begin(), preview_copy.end(), sort_by_score);
      const size_t preview = std::min((size_t)5, preview_copy.size());
      for (size_t i = 0; i < preview; i++) {
        char meta_msg[224];
        snprintf(meta_msg, sizeof(meta_msg),
                 "MOBI: cover meta cand[%u] rec=%u dim=%dx%d score=%d",
                 (unsigned)i, (unsigned)preview_copy[i].record_idx,
                 preview_copy[i].width, preview_copy[i].height,
                 (int)preview_copy[i].score);
        reporter->PrintStatus(meta_msg);
      }
    }
    if (try_decode_ranked(meta_ranked))
      return 0;
  }

  std::vector<MobiCoverCandidate> ranked;
  ranked.reserve(candidates.size() + 64);
  for (size_t i = 0; i < candidates.size(); i++) {
    MobiCoverCandidate c;
    if (!ProbeRecordAsCoverCandidate(&reader, offsets, candidates[i], &c))
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
    if (!ProbeRecordAsCoverCandidate(&reader, offsets, i, &c))
      continue;
    c.score -= 24.0f; // Prefer explicit/proximal candidates over global scan.
    ranked.push_back(c);
  }

  if (!ranked.empty() && try_decode_ranked(ranked))
    return 0;

  SaveMobiCoverMetaMiss(meta_cache_path);
  if (reporter)
    reporter->PrintStatus("MOBI: cover scan none");
  return 5;
}

bool mobi_parse_offsets(const std::string &raw, std::vector<u32> *offsets) {
  return ParseMobiOffsets(raw, offsets);
}

size_t mobi_find_image_start_offset(const u8 *data, size_t len) {
  return FindImageStartOffset(data, len);
}
