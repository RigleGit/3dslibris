/*
    3dslibris - book_inline_image.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Decodes inline images referenced by EPUB/FB2 content.
    - Maintains bounded caches for decoded/scaled image payloads.
    - Provides safe limits for image bytes/dimensions on 3DS memory budget.
*/

#include "book/book.h"

#include "app/app.h"

#include "base64_utils.h"
#include "debug_log.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/file_read_utils.h"
#include "formats/mobi/mobi.h"
#include "formats/mobi/mobi_record_scan.h"
#include "path_utils.h"
#include "stb_image.h"
#include "string_utils.h"
#include "ui/text.h"
#include "unzip.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <utility>

namespace {
static const size_t kInlineImageCacheMaxBytes = 1024 * 1024;
static const size_t kFb2InlineImageMaxBytes = 4 * 1024 * 1024;
static const size_t kFb2InlineImageMaxTotalBytes = 12 * 1024 * 1024;
static const size_t kEpubInlineImageMaxBytes = 4 * 1024 * 1024;
static const size_t kEpubInlineImageProbeBytes = 64 * 1024;
static const size_t kSvgWrapperMaxBytes = 512 * 1024;
static const size_t kMobiInlineFileMaxBytes = 64 * 1024 * 1024;
static const size_t kMobiInlineRecordMaxBytes = 8 * 1024 * 1024;

static std::string NormalizeZipEntryKey(const std::string &path) {
  std::string normalized;
  normalized.reserve(path.size());
  for (size_t i = 0; i < path.size(); i++) {
    char c = path[i];
    if (c == '\\')
      c = '/';
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    normalized.push_back(c);
  }
  return normalized;
}

static bool LocateBookInlineZipEntry(
    unzFile uf, bool *index_built,
    std::unordered_map<std::string, unsigned long> *offsets,
    const std::string &path) {
  if (!uf || !index_built || !offsets || path.empty())
    return false;

  if (!*index_built) {
    *index_built = true;
    int rc = unzGoToFirstFile(uf);
    while (rc == UNZ_OK) {
      char fname[1024];
      unz_file_info fi;
      if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL,
                                0) == UNZ_OK) {
        std::string key = NormalizeZipEntryKey(fname);
        if (!key.empty() && offsets->find(key) == offsets->end()) {
          (*offsets)[key] = (unsigned long)unzGetOffset(uf);
        }
      }
      rc = unzGoToNextFile(uf);
    }
  }

  std::string key = NormalizeZipEntryKey(path);
  std::unordered_map<std::string, unsigned long>::const_iterator hit =
      offsets->find(key);
  if (hit != offsets->end() &&
      unzSetOffset(uf, (uLong)hit->second) == UNZ_OK) {
    return true;
  }

  return unzLocateFile(uf, path.c_str(), 2) == UNZ_OK;
}

static bool ReadZipEntryBinaryPrefix(
    unzFile uf, bool *index_built,
    std::unordered_map<std::string, unsigned long> *offsets,
    const std::string &path, std::vector<u8> *out,
                                     size_t max_prefix_bytes) {
  if (!out || !uf || path.empty() || max_prefix_bytes == 0)
    return false;
  out->clear();

  if (!LocateBookInlineZipEntry(uf, index_built, offsets, path))
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
      fi.uncompressed_size == 0 || fi.uncompressed_size > kEpubInlineImageMaxBytes) {
    unzCloseCurrentFile(uf);
    return false;
  }

  size_t want = std::min((size_t)fi.uncompressed_size, max_prefix_bytes);
  out->resize(want);
  int total = 0;
  while (total < (int)want) {
    int n = unzReadCurrentFile(uf, out->data() + total,
                               (unsigned int)(want - (size_t)total));
    if (n < 0) {
      unzCloseCurrentFile(uf);
      out->clear();
      return false;
    }
    if (n == 0)
      break;
    total += n;
  }
  unzCloseCurrentFile(uf);
  if (total <= 0) {
    out->clear();
    return false;
  }
  out->resize((size_t)total);
  return true;
}

static bool LooksLikeSvgWrapper(const std::string &path_hint,
                                const std::vector<u8> &buf) {
  return epub_image_utils::LooksLikeSvgWrapper(path_hint, buf);
}

static bool
ResolveSvgWrapperImage(unzFile uf, const std::string &svg_path,
                       const std::vector<u8> &svg_buf, std::vector<u8> *out,
                       std::string *resolved_path = NULL, App *app = NULL) {
  if (!out || !uf || svg_buf.empty() || svg_buf.size() > kSvgWrapperMaxBytes) {
    if (app)
      DBG_LOG(app, "EPUB: inline SVG wrapper skip (size/empty)");
    return false;
  }
  bool ok = epub_image_utils::ResolveSvgWrapperImage(
      uf, svg_path, svg_buf, out, kEpubInlineImageMaxBytes, resolved_path);
  if (!ok && app) {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper unresolved path=%s",
             svg_path.c_str());
    DBG_LOG(app, msg);
  }
  return ok;
}

static bool ReadFileSlice(const std::string &path, u32 start, u32 end,
                          std::vector<u8> *out) {
  if (!out || path.empty() || end <= start)
    return false;
  out->clear();
  size_t len = (size_t)(end - start);
  if (len == 0 || len > kMobiInlineRecordMaxBytes)
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
} // namespace

u16 Book::RegisterInlineImage(const std::string &path) {
  if (path.empty())
    return 0;
  std::unordered_map<std::string, u16>::const_iterator it =
      inline_image_path_index.find(path);
  if (it != inline_image_path_index.end())
    return it->second;
  if (inline_images.size() >= 65535)
    return 0;
  InlineImageEntry entry;
  entry.path = path;
  inline_images.push_back(entry);
  u16 id = (u16)(inline_images.size() - 1);
  inline_image_path_index[path] = id;
  return id;
}

const std::string *Book::GetInlineImagePath(u16 id) const {
  if (id >= inline_images.size())
    return NULL;
  return &inline_images[id].path;
}

u32 Book::GetInlineImageCount() const {
  return (u32)inline_images.size();
}

void Book::SetInlineImageFollowTextLines(u16 id, u8 lines) {
  if (id >= inline_images.size())
    return;
  inline_images[id].follow_text_lines = lines;
}

u8 Book::GetInlineImageFollowTextLines(u16 id) const {
  if (id >= inline_images.size())
    return 0;
  return inline_images[id].follow_text_lines;
}

void Book::ClearInlineImageCache() {
  inline_image_cache.clear();
  inline_image_cache_bytes = 0;
}

void Book::ClearInlineImages() {
  inline_images.clear();
  inline_image_path_index.clear();
  inline_image_probe_uf = NULL;
  inline_image_zip_index_built = false;
  inline_image_zip_offsets.clear();
  mobi_inline_index_ready = false;
  mobi_first_image_index = 0;
  mobi_record_offsets.clear();
  fb2_inline_images.clear();
  fb2_inline_images_bytes = 0;
  ClearInlineImageCache();
}

void Book::SetInlineImageProbeZip(void *uf) { inline_image_probe_uf = uf; }

bool Book::StoreFb2InlineImage(const std::string &id,
                               const std::string &base64_data) {
  if (id.empty() || base64_data.empty())
    return false;

  std::string key = id;
  if (!key.empty() && key[0] == '#')
    key.erase(0, 1);
  if (key.empty())
    return false;

  std::vector<u8> decoded;
  if (!DecodeBase64Bytes(base64_data, &decoded, kFb2InlineImageMaxBytes))
    return false;

  size_t old_size = 0;
  std::unordered_map<std::string, std::vector<u8>>::iterator it =
      fb2_inline_images.find(key);
  if (it != fb2_inline_images.end())
    old_size = it->second.size();

  if (fb2_inline_images_bytes - old_size + decoded.size() >
      kFb2InlineImageMaxTotalBytes) {
    return false;
  }

  fb2_inline_images_bytes = fb2_inline_images_bytes - old_size + decoded.size();
  fb2_inline_images[key] = std::move(decoded);
  return true;
}

bool Book::LoadInlineImageSource(u16 image_id, std::vector<u8> *out,
                                 std::string *resolved_path) {
  if (!out)
    return false;
  out->clear();
  if (resolved_path)
    resolved_path->clear();
  if (image_id >= inline_images.size())
    return false;

  App *app = GetApp();
  const std::string &image_path = inline_images[image_id].path;
  if (image_path.empty())
    return false;

  if (image_path.compare(0, 4, "fb2:") == 0) {
    std::string key = image_path.substr(4);
    if (!key.empty() && key[0] == '#')
      key.erase(0, 1);
    std::unordered_map<std::string, std::vector<u8>>::const_iterator hit =
        fb2_inline_images.find(key);
    if (hit == fb2_inline_images.end() || hit->second.empty())
      return false;
    *out = hit->second;
    if (resolved_path)
      *resolved_path = image_path;
    return true;
  }

  u16 mobi_recindex = 0;
  if (mobi_parse_inline_image_path(image_path, &mobi_recindex)) {
    if (!mobi_inline_index_ready) {
      mobi_inline_index_ready = true;
      mobi_first_image_index = 0;
      mobi_record_offsets.clear();

      std::string mobipath = foldername + "/" + filename;
      std::string raw;
      if (file_read_utils::ReadPathToStringLimited(mobipath.c_str(), &raw,
                                                   kMobiInlineFileMaxBytes) &&
          mobi_parse_offsets(raw, &mobi_record_offsets) &&
          mobi_record_offsets.size() >= 3) {
        const u8 *data = (const u8 *)raw.data();
        const u32 rec0_start = mobi_record_offsets[0];
        const u32 rec0_end = mobi_record_offsets[1];
        if (rec0_end > rec0_start && rec0_end - rec0_start >= 16) {
          const u8 *rec0 = data + rec0_start;
          const size_t rec0_len = (size_t)(rec0_end - rec0_start);
          u32 text_rec_count = (u32)rec0[8] << 8 | (u32)rec0[9];
          u32 first_non_book_index = 0;
          if (rec0_len >= 24 && memcmp(rec0 + 16, "MOBI", 4) == 0) {
            const u8 *mobi = rec0 + 16;
            const u32 mobi_len = (u32)mobi[4] << 24 | (u32)mobi[5] << 16 |
                                 (u32)mobi[6] << 8 | (u32)mobi[7];
            if (mobi_len >= 0x70 && rec0_len >= 16 + (size_t)0x70) {
              mobi_first_image_index =
                  (u32)mobi[0x6C] << 24 | (u32)mobi[0x6D] << 16 |
                  (u32)mobi[0x6E] << 8 | (u32)mobi[0x6F];
            }
            if (mobi_len >= 0x84 && rec0_len >= 16 + (size_t)0x84) {
              first_non_book_index =
                  (u32)mobi[0x80] << 24 | (u32)mobi[0x81] << 16 |
                  (u32)mobi[0x82] << 8 | (u32)mobi[0x83];
            }
          }

          const u32 rec_count = (u32)mobi_record_offsets.size() - 1;
          const u32 max_text_records = rec_count > 1 ? rec_count - 2 : 0;
          if (text_rec_count == 0 || text_rec_count > max_text_records)
            text_rec_count = max_text_records;
          if (first_non_book_index > 1) {
            const u32 boundary = first_non_book_index - 1;
            if (boundary > 0 && boundary < text_rec_count)
              text_rec_count = boundary;
          }

          u32 detected_first_image_index = 0;
          const u32 probe_start = text_rec_count + 1;
          if (probe_start < rec_count) {
            const u32 remaining = rec_count - probe_start;
            const u32 probe_end =
                std::min<u32>(rec_count, probe_start +
                                             mobi_record_scan::FirstImageProbeLimit(
                                                 remaining));
            for (u32 rec = probe_start; rec < probe_end; rec++) {
              const u32 start = mobi_record_offsets[(size_t)rec];
              const u32 end = mobi_record_offsets[(size_t)rec + 1];
              if (end <= start)
                continue;
              const size_t len = (size_t)(end - start);
              if (len < 32 || len > kMobiInlineRecordMaxBytes)
                continue;
              if (mobi_find_image_start_offset(data + start, len) != SIZE_MAX) {
                detected_first_image_index = rec;
                break;
              }
            }
          }

          if (mobi_first_image_index == 0 && text_rec_count + 1 < rec_count)
            mobi_first_image_index = text_rec_count + 1;
          if (detected_first_image_index > 0)
            mobi_first_image_index = detected_first_image_index;
          if (mobi_first_image_index >= rec_count)
            mobi_first_image_index = 0;
        }
      }
    }

    if (mobi_first_image_index == 0 || mobi_record_offsets.size() < 3)
      return false;

    u32 record_index = mobi_first_image_index + (u32)(mobi_recindex - 1);
    if (record_index + 1 >= mobi_record_offsets.size())
      return false;

    std::string mobipath = foldername + "/" + filename;
    if (!ReadFileSlice(mobipath, mobi_record_offsets[(size_t)record_index],
                       mobi_record_offsets[(size_t)record_index + 1], out)) {
      return false;
    }

    size_t image_off = mobi_find_image_start_offset(out->data(), out->size());
    if (image_off == SIZE_MAX)
      return false;
    if (image_off > 0)
      out->erase(out->begin(), out->begin() + image_off);
    if (out->empty())
      return false;
    if (resolved_path)
      *resolved_path = image_path;
    return true;
  }

  std::string epubpath = foldername + "/" + filename;
  unzFile uf = (unzFile)inline_image_probe_uf;
  bool close_uf = false;
  if (!uf) {
    uf = unzOpen(epubpath.c_str());
    close_uf = true;
  }
  if (!uf)
    return false;

  if (!LocateBookInlineZipEntry(uf, &inline_image_zip_index_built,
                                &inline_image_zip_offsets, image_path)) {
    if (close_uf)
      unzClose(uf);
    return false;
  }
  if (unzOpenCurrentFile(uf) != UNZ_OK) {
    if (close_uf)
      unzClose(uf);
    return false;
  }

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
      fi.uncompressed_size == 0 ||
      fi.uncompressed_size > kEpubInlineImageMaxBytes) {
    unzCloseCurrentFile(uf);
    if (close_uf)
      unzClose(uf);
    return false;
  }

  out->resize(fi.uncompressed_size);
  int bytes_read = unzReadCurrentFile(uf, out->data(), fi.uncompressed_size);
  unzCloseCurrentFile(uf);
  if (bytes_read <= 0) {
    if (close_uf)
      unzClose(uf);
    out->clear();
    return false;
  }
  out->resize((size_t)bytes_read);

  if (resolved_path)
    *resolved_path = image_path;

  int info_w = 0, info_h = 0, info_c = 0;
  if (stbi_info_from_memory(out->data(), (int)out->size(), &info_w, &info_h,
                            &info_c) != 0) {
    if (close_uf)
      unzClose(uf);
    return true;
  }

  bool is_svg_wrapper = LooksLikeSvgWrapper(image_path, *out);
  std::vector<u8> resolved_svg_image;
  std::string resolved_svg_path;
  if (is_svg_wrapper && app) {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper detected id=%u %s",
             (unsigned)image_id, image_path.c_str());
    DBG_LOG(app, msg);
  }
  if (is_svg_wrapper &&
      ResolveSvgWrapperImage(uf, image_path, *out, &resolved_svg_image,
                             &resolved_svg_path, app)) {
    out->swap(resolved_svg_image);
    if (resolved_path && !resolved_svg_path.empty())
      *resolved_path = resolved_svg_path;
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg),
               "EPUB: inline SVG wrapper resolved id=%u src=%s bytes=%u",
               (unsigned)image_id,
               resolved_svg_path.empty() ? "(unknown)"
                                         : resolved_svg_path.c_str(),
               (unsigned)out->size());
      DBG_LOG(app, msg);
    }
    if (close_uf)
      unzClose(uf);
    return true;
  }
  if (close_uf)
    unzClose(uf);
  if (is_svg_wrapper) {
    if (app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper unresolved id=%u %s",
               (unsigned)image_id, image_path.c_str());
      DBG_LOG(app, msg);
    }
    out->clear();
    return false;
  }
  return true;
}

bool Book::EnsureInlineImageMetadata(u16 image_id, InlineImageMetadata *out) {
  if (out) {
    out->width = 0;
    out->height = 0;
    out->ok = false;
  }
  if (image_id >= inline_images.size())
    return false;

  InlineImageEntry &entry = inline_images[image_id];
  if (!entry.metadata_probed) {
    entry.metadata_probed = true;
    entry.metadata_ok = false;
    entry.source_width = 0;
    entry.source_height = 0;

    const std::string &image_path = entry.path;
    std::vector<u8> compressed;
    std::string resolved_path;
    bool need_full_load = false;
    bool already_loaded_full = false;

    if (image_path.compare(0, 4, "fb2:") == 0 ||
        image_path.compare(0, 14, "mobi:recindex:") == 0) {
      if (LoadInlineImageSource(image_id, &compressed, &resolved_path) &&
          !compressed.empty()) {
        already_loaded_full = true;
      }
    } else {
      unzFile uf = (unzFile)inline_image_probe_uf;
      bool close_uf = false;
      if (!uf) {
        std::string epubpath = foldername + "/" + filename;
        uf = unzOpen(epubpath.c_str());
        close_uf = true;
      }
      if (uf) {
        if (ReadZipEntryBinaryPrefix(uf, &inline_image_zip_index_built,
                                     &inline_image_zip_offsets, image_path,
                                     &compressed, kEpubInlineImageProbeBytes)) {
          need_full_load = LooksLikeSvgWrapper(image_path, compressed);
          if (!need_full_load) {
            resolved_path = image_path;
          }
        }
        if (close_uf)
          unzClose(uf);
      }
    }

    if (!already_loaded_full && need_full_load &&
        (!LoadInlineImageSource(image_id, &compressed, &resolved_path) ||
         compressed.empty())) {
      compressed.clear();
    }

    if (!compressed.empty()) {
      int info_w = 0, info_h = 0, info_c = 0;
      bool have_info =
          stbi_info_from_memory(compressed.data(), (int)compressed.size(),
                                &info_w, &info_h, &info_c) != 0;
      if ((!have_info || info_w <= 0 || info_h <= 0) &&
          image_path.compare(0, 14, "mobi:recindex:") == 0) {
        // Some MOBI photo records decode fine but fail the lightweight probe.
        // Fall back to a full decode once so pagination still gets dimensions.
        int load_w = 0, load_h = 0, load_c = 0;
        unsigned char *pixels = stbi_load_from_memory(
            compressed.data(), (int)compressed.size(), &load_w, &load_h, &load_c,
            0);
        if (pixels) {
          stbi_image_free(pixels);
          info_w = load_w;
          info_h = load_h;
          have_info = true;
        }
      }
      if (have_info && info_w > 0 && info_h > 0 &&
          ((long long)info_w * (long long)info_h <= 1500000LL)) {
        entry.metadata_ok = true;
        entry.source_width = info_w;
        entry.source_height = info_h;
        if (!resolved_path.empty() &&
            resolved_path.compare(0, 5, "data:") != 0) {
          entry.path = resolved_path;
        }
      }
    }
  }

  if (out) {
    out->width = entry.source_width;
    out->height = entry.source_height;
    out->ok = entry.metadata_ok;
  }
  return entry.metadata_ok;
}

bool Book::GetInlineImageMetadata(u16 id, InlineImageMetadata *out) {
  return EnsureInlineImageMetadata(id, out);
}

bool Book::PlanInlineImageLayout(Text *ts, u16 image_id, int current_screen,
                                 int pen_x, int pen_y, bool line_began,
                                 InlineImageContext image_context,
                                 InlineImageLayoutPlan *out) {
  if (!ts || !out)
    return false;

  InlineImageMetadata meta{};
  EnsureInlineImageMetadata(image_id, &meta);

  InlineImageLayoutRequest req{};
  req.screen_width = 240;
  req.screen_height = (current_screen == 0) ? 400 : 320;
  req.margin_left = ts->margin.left;
  req.margin_right = ts->margin.right;
  req.margin_top = ts->margin.top;
  req.margin_bottom = (current_screen == 0) ? ts->margin.bottom
                                            : std::min(ts->margin.bottom, 16);
  req.line_height = ts->GetHeight();
  req.linespacing = ts->linespacing;
  req.pen_x = pen_x;
  req.pen_y = pen_y;
  req.line_began = line_began;
  req.image_context = image_context;
  req.current_screen = current_screen;
  req.follow_text_lines = GetInlineImageFollowTextLines(image_id);

  *out = ::PlanInlineImageLayout(req, meta);
  return true;
}

bool Book::DrawInlineImage(Text *ts, u16 image_id,
                           const InlineImageLayoutPlan *plan_ptr) {
  if (!ts)
    return false;

  InlineImageLayoutPlan local_plan{};
  const bool left_screen = (ts->GetScreen() == ts->screenleft);
  const int current_screen = left_screen ? 0 : 1;
  if (!plan_ptr) {
    if (!PlanInlineImageLayout(ts, image_id, current_screen, ts->GetPenX(),
                               ts->GetPenY(), ts->linebegan,
                               INLINE_IMAGE_CONTEXT_DEFAULT,
                               &local_plan))
      return false;
    plan_ptr = &local_plan;
  }
  const InlineImageLayoutPlan &plan = *plan_ptr;

  const int screen_w = 240;
  const int screen_h = left_screen ? 400 : 320;
  const int text_w = screen_w - ts->margin.left - ts->margin.right;
  const int line_height = ts->GetHeight();
  const int line_top = ts->GetPenY() - line_height;
  u16 bg565 = ts->GetBgColor();

  int draw_w = plan.draw_width;
  int draw_h = plan.draw_height;
  int start_x = 0;
  int start_y = 0;

  // PAGE mode needs source dimensions to compute draw size before cache
  // lookup. Use metadata (cheap) first; actual pixel decode only on miss.
  int page_imgW = 0, page_imgH = 0;
  bool need_page_recompute =
      (plan.mode == INLINE_IMAGE_LAYOUT_PAGE || draw_w <= 0 || draw_h <= 0);

  if (need_page_recompute) {
    InlineImageMetadata meta{};
    EnsureInlineImageMetadata(image_id, &meta);
    page_imgW = meta.width;
    page_imgH = meta.height;
    if (page_imgW <= 0 || page_imgH <= 0) {
      // Metadata unavailable; falls back after decode below.
      page_imgW = 0;
      page_imgH = 0;
    } else {
      const int pad = 2;
      const int avail_w = screen_w - (pad * 2);
      const int avail_h = screen_h - (pad * 2);
      int sx = (avail_w * 1024) / std::max(1, page_imgW);
      int sy = (avail_h * 1024) / std::max(1, page_imgH);
      int scale = std::min(sx, sy);
      if (scale > 1024)
        scale = 1024;
      draw_w = std::max(1, (page_imgW * scale + 512) / 1024);
      draw_h = std::max(1, (page_imgH * scale + 512) / 1024);
      start_x = pad + (avail_w - draw_w) / 2;
      start_y = pad + (avail_h - draw_h) / 2;
    }
  }

  if (!need_page_recompute) {
    if (plan.mode == INLINE_IMAGE_LAYOUT_INLINE) {
      start_x = ts->GetPenX();
      start_y = line_top;
    } else {
      start_x = ts->margin.left + (text_w - draw_w) / 2;
      start_y = line_top;
    }
  }

  auto blit_pixels = [&](const InlineImageCacheEntry &entry, int dst_x,
                          int dst_y) {
    u16 *dst = ts->GetScreen();
    // 3DS framebuffer is column-major: stride == display.height, not width.
    const int stride = ts->display.height;
    for (int y = 0; y < entry.height; y++) {
      int dy = dst_y + y;
      if (dy < 0 || dy >= screen_h)
        continue;

      int draw_x = dst_x;
      int draw_w_local = entry.width;
      int src_x = 0;
      if (draw_x < 0) {
        src_x = -draw_x;
        draw_w_local -= src_x;
        draw_x = 0;
      }
      if (draw_x + draw_w_local > screen_w)
        draw_w_local = screen_w - draw_x;
      if (draw_w_local <= 0)
        continue;

      const u16 *src_row = entry.pixels.data() + (y * entry.width) + src_x;
      u16 *dst_row = dst + (dy * stride) + draw_x;
      memcpy(dst_row, src_row, draw_w_local * sizeof(u16));
    }
  };

  // LRU cache lookup before image decode; skip when dimensions unknown.
  if (draw_w > 0 && draw_h > 0) {
    for (std::list<InlineImageCacheEntry>::iterator it =
             inline_image_cache.begin();
         it != inline_image_cache.end(); ++it) {
      if (it->image_id == image_id && it->screen_h == (u16)screen_h &&
          it->bg565 == bg565 && it->layout_mode == (u8)plan.mode &&
          it->width == draw_w && it->height == draw_h) {
        inline_image_cache.splice(inline_image_cache.begin(),
                                  inline_image_cache, it);
        blit_pixels(inline_image_cache.front(), start_x, start_y);
        return true;
      }
    }
  }

  std::vector<u8> compressed;
  if (!LoadInlineImageSource(image_id, &compressed, NULL) || compressed.empty())
    return false;

  int imgW = 0, imgH = 0, channels = 0;
  int loaded_channels = 4;
  unsigned char *pixels = stbi_load_from_memory(
      compressed.data(), (int)compressed.size(), &imgW, &imgH, &channels, 4);
  if (!pixels) {
    // Some valid JPEGs fail the 4-channel expansion path on-device. Retry with
    // RGB data and treat the image as fully opaque during the scale/blit pass.
    loaded_channels = 3;
    pixels = stbi_load_from_memory(compressed.data(), (int)compressed.size(),
                                   &imgW, &imgH, &channels, 3);
  }
  if (!pixels)
    return false;

  // PAGE fallback: metadata was unavailable, compute from decoded pixels.
  if (need_page_recompute && page_imgW == 0 && page_imgH == 0) {
    const int pad = 2;
    const int avail_w = screen_w - (pad * 2);
    const int avail_h = screen_h - (pad * 2);
    int sx = (avail_w * 1024) / std::max(1, imgW);
    int sy = (avail_h * 1024) / std::max(1, imgH);
    int scale = std::min(sx, sy);
    if (scale > 1024)
      scale = 1024;
    draw_w = std::max(1, (imgW * scale + 512) / 1024);
    draw_h = std::max(1, (imgH * scale + 512) / 1024);
    start_x = pad + (avail_w - draw_w) / 2;
    start_y = pad + (avail_h - draw_h) / 2;
  }

  // Guard: cap draw buffer to 1.5M pixels (same as EnsureInlineImageMetadata).
  if ((long long)draw_w * (long long)draw_h > 1500000LL) {
    stbi_image_free(pixels);
    return false;
  }

  InlineImageCacheEntry entry;
  entry.image_id = image_id;
  entry.screen_h = (u16)screen_h;
  entry.bg565 = bg565;
  entry.layout_mode = (u8)plan.mode;
  entry.width = (u16)draw_w;
  entry.height = (u16)draw_h;
  entry.pixels.resize((size_t)draw_w * (size_t)draw_h);

  u8 bg_r5 = (bg565 >> 11) & 0x1F;
  u8 bg_g6 = (bg565 >> 5) & 0x3F;
  u8 bg_b5 = bg565 & 0x1F;
  u8 bg_r8 = (bg_r5 << 3) | (bg_r5 >> 2);
  u8 bg_g8 = (bg_g6 << 2) | (bg_g6 >> 4);
  u8 bg_b8 = (bg_b5 << 3) | (bg_b5 >> 2);

  for (int y = 0; y < draw_h; y++) {
    int src_y = (y * imgH) / std::max(1, draw_h);
    if (src_y >= imgH)
      src_y = imgH - 1;
    for (int x = 0; x < draw_w; x++) {
      int src_x = (x * imgW) / std::max(1, draw_w);
      if (src_x >= imgW)
        src_x = imgW - 1;
      unsigned char *px =
          &pixels[(src_y * imgW + src_x) * loaded_channels];

      u8 r8 = px[0];
      u8 g8 = px[1];
      u8 b8 = px[2];
      u8 a8 = (loaded_channels >= 4) ? px[3] : 255;
      if (a8 < 255) {
        r8 = (u8)((r8 * a8 + bg_r8 * (255 - a8) + 127) / 255);
        g8 = (u8)((g8 * a8 + bg_g8 * (255 - a8) + 127) / 255);
        b8 = (u8)((b8 * a8 + bg_b8 * (255 - a8) + 127) / 255);
      }

      u16 r = (r8 >> 3) & 0x1F;
      u16 g = (g8 >> 2) & 0x3F;
      u16 b = (b8 >> 3) & 0x1F;
      entry.pixels[(y * draw_w) + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);

  const size_t entry_bytes = entry.pixels.size() * sizeof(u16);
  if (entry_bytes <= kInlineImageCacheMaxBytes) {
    while (!inline_image_cache.empty() &&
           inline_image_cache_bytes + entry_bytes > kInlineImageCacheMaxBytes) {
      inline_image_cache_bytes -=
          inline_image_cache.back().pixels.size() * sizeof(u16);
      inline_image_cache.pop_back();
    }
    inline_image_cache.push_front(std::move(entry));
    inline_image_cache_bytes += entry_bytes;
    blit_pixels(inline_image_cache.front(), start_x, start_y);
  } else {
    blit_pixels(entry, start_x, start_y);
  }

  return true;
}
