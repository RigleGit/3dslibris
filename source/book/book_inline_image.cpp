/*
    3dslibris - book_inline_image.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Decodes inline images referenced by EPUB/FB2 content.
    - Maintains bounded caches for decoded/scaled image payloads.
    - Provides safe limits for image bytes/dimensions on 3DS memory budget.
*/

#include "book/book.h"
#include "shared/screen_dimensions.h"

#include "book/inline_image_page_layout_utils.h"
#include "book/inline_image_screen_layout.h"
#include "shared/base64_utils.h"
#include "shared/debug_log.h"
#include "formats/common/epub_image_utils.h"
#include "formats/common/raster_image_decode.h"
#include "formats/mobi/mobi.h"
#include "formats/mobi/mobi_record_scan.h"
#include "shared/orientation_utils.h"
#include "shared/path_constants.h"
#include "shared/status_reporter.h"
#include "stb_image.h"
#include "shared/string_utils.h"
#include "ui/text.h"
#include "minizip/unzip.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <utility>


namespace {
static const size_t kInlineImageCacheMaxBytes = 1024 * 1024;
static const size_t kInlineImageCacheMaxEntries = 64;
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
  if (hit != offsets->end() && unzSetOffset(uf, (uLong)hit->second) == UNZ_OK) {
    return true;
  }

  return unzLocateFile(uf, path.c_str(), 2) == UNZ_OK;
}

static bool ReadZipEntryBinaryPrefix(
    unzFile uf, bool *index_built,
    std::unordered_map<std::string, unsigned long> *offsets,
    const std::string &path, image_memory::Bytes *out, size_t max_prefix_bytes) {
  if (!out || !uf || path.empty() || max_prefix_bytes == 0)
    return false;
  out->clear();

  if (!LocateBookInlineZipEntry(uf, index_built, offsets, path))
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
      fi.uncompressed_size == 0 ||
      fi.uncompressed_size > kEpubInlineImageMaxBytes) {
    unzCloseCurrentFile(uf);
    return false;
  }

  size_t want = std::min((size_t)fi.uncompressed_size, max_prefix_bytes);
  if(!out->resize(want)) {unzCloseCurrentFile(uf);return false;}
  int total = 0;
  while (total < (int)want) {
    int n = unzReadCurrentFile(uf, out->mutable_data() + total,
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
                                const image_memory::Bytes &buf) {
  return epub_image_utils::LooksLikeSvgWrapper(path_hint, buf);
}

static bool ResolveSvgWrapperImage(unzFile uf, const std::string &svg_path,
                                   const image_memory::Bytes &svg_buf,
                                   image_memory::Bytes *out,
                                   std::string *resolved_path = NULL,
                                   IStatusReporter *reporter = NULL) {
  if (!out || !uf || svg_buf.empty() || svg_buf.size() > kSvgWrapperMaxBytes) {
    if (reporter)
      DBG_LOG(reporter, "EPUB: inline SVG wrapper skip (size/empty)");
    return false;
  }
  bool ok = epub_image_utils::ResolveSvgWrapperImage(
      uf, svg_path, svg_buf, out, kEpubInlineImageMaxBytes, resolved_path);
  if (!ok && reporter) {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper unresolved path=%s",
             svg_path.c_str());
    DBG_LOG(reporter, msg);
  }
  return ok;
}

static bool ReadFileSlice(const std::string &path, u32 start, u32 end,
                          image_memory::Bytes *out) {
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
    if(out->resize(len))
      ok = fread(out->mutable_data(), 1, len, fp) == len;
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

void Book::SetInlineImageAuthorMaxWidth(u16 id, int px) {
  if (id >= inline_images.size())
    return;
  inline_images[id].author_max_width_px = px;
}

void Book::ClearInlineImageCache() {
  inline_image_cache.clear();
  inline_image_cache_index.clear();
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

void Book::SetInlineImageProbeZip(void *uf) {
  inline_image_probe_uf = uf;
}

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

bool Book::LoadInlineImageSource(u16 image_id, image_memory::Bytes *out,
                                 std::string *resolved_path) {
  if (!out)
    return false;
  out->clear();
  if (resolved_path)
    resolved_path->clear();
  if (image_id >= inline_images.size())
    return false;

  IStatusReporter *reporter = GetStatusReporter();
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
    out->Borrow(hit->second.data(),hit->second.size());
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

      const std::string mobipath=foldername+"/"+filename;
      FILE *fp=fopen(mobipath.c_str(),"rb");
      if(!fp) {mobi_inline_index_ready=false;return false;}
      // Read the Palm table and bounded record prefixes, never the whole book.
      unsigned char header[78];
      bool ok=fseek(fp,0,SEEK_END)==0;
      const long file_size=ok ? ftell(fp) : -1;
      ok=file_size>=78 && (size_t)file_size<=kMobiInlineFileMaxBytes &&
          fseek(fp,0,SEEK_SET)==0 && fread(header,1,sizeof(header),fp)==sizeof(header);
      const unsigned records=ok ? mobi_record_scan::ReadBE16(header+76) : 0;
      const size_t table_end=78+(size_t)records*8;
      ok=ok && records>=2 && table_end<=(size_t)file_size &&
          mobi_record_offsets.resize(records+1);
      for(unsigned i=0;ok && i<records;++i) {
        unsigned char entry[8];
        ok=fread(entry,1,sizeof(entry),fp)==sizeof(entry);
        if(!ok) break;
        const u32 offset=mobi_record_scan::ReadBE32(entry);
        ok=offset>=table_end && offset<(u32)file_size &&
            (!i || offset>=mobi_record_offsets[i-1]);
        mobi_record_offsets.mutable_data()[i]=offset;
      }
      unsigned char rec0[16+0x84]={};
      if(ok) {
        mobi_record_offsets.mutable_data()[records]=(u32)file_size;
        const size_t length=std::min(sizeof(rec0),(size_t)(mobi_record_offsets[1]-mobi_record_offsets[0]));
        ok=length>=16 && fseek(fp,mobi_record_offsets[0],SEEK_SET)==0 &&
            fread(rec0,1,length,fp)==length;
        if(ok) {
          u32 text_count=mobi_record_scan::ReadBE16(rec0+8);
          u32 first_non_book=0;
          if(length>=24 && memcmp(rec0+16,"MOBI",4)==0) {
            const u8 *mobi=rec0+16;
            const u32 mobi_length=mobi_record_scan::ReadBE32(mobi+4);
            if(mobi_length>=0x70 && length>=16+0x70)
              mobi_first_image_index=mobi_record_scan::ReadBE32(mobi+0x6c);
            if(mobi_length>=0x84 && length>=16+0x84)
              first_non_book=mobi_record_scan::ReadBE32(mobi+0x80);
          }
          const u32 max_text=records>1 ? records-2 : 0;
          if(!text_count || text_count>max_text) text_count=max_text;
          if(first_non_book>1 && first_non_book-1<text_count) text_count=first_non_book-1;
          const u32 probe_start=text_count+1;
          image_memory::Bytes prefix;
          const u32 probe_end=probe_start+mobi_record_scan::FirstImageProbeLimit(
              records>probe_start ? records-probe_start : 0);
          for(u32 record=probe_start;record<probe_end;++record) {
            const u32 begin=mobi_record_offsets[record],end=mobi_record_offsets[record+1];
            const size_t length=(size_t)(end-begin);
            if(length<32 || length>kMobiInlineRecordMaxBytes) continue;
            const size_t want=std::min(length,(size_t)16384);
            if(!prefix.resize(want) || fseek(fp,begin,SEEK_SET)!=0 ||
                fread(prefix.mutable_data(),1,want,fp)!=want) {ok=false;break;}
            if(mobi_find_image_start_offset(prefix.data(),want)!=SIZE_MAX) {
              mobi_first_image_index=record;break;
            }
          }
          if(!mobi_first_image_index && probe_start<records) mobi_first_image_index=probe_start;
          if(mobi_first_image_index>=records) mobi_first_image_index=0;
        }
      }
      fclose(fp);
      if(!ok) {
        mobi_record_offsets.clear();mobi_first_image_index=0;
        mobi_inline_index_ready=false;return false;
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
      out->RemovePrefix(image_off);
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

  if(!out->resize(fi.uncompressed_size)) {
    unzCloseCurrentFile(uf);if(close_uf) unzClose(uf);return false;
  }
  size_t total=0;
  bool ok=true;
  while(total<out->size()) {
    int n=unzReadCurrentFile(uf,out->mutable_data()+total,(unsigned)(out->size()-total));
    if(n<=0) {ok=false;break;}total+=(size_t)n;
  }
  if(unzCloseCurrentFile(uf)!=UNZ_OK) ok=false;
  if(!ok) {
    if(close_uf) unzClose(uf);
    out->clear();return false;
  }

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
  image_memory::Bytes resolved_svg_image;
  std::string resolved_svg_path;
  if (is_svg_wrapper && reporter) {
    char msg[192];
    snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper detected id=%u %s",
             (unsigned)image_id, image_path.c_str());
    DBG_LOG(reporter, msg);
  }
  if (is_svg_wrapper &&
      ResolveSvgWrapperImage(uf, image_path, *out, &resolved_svg_image,
                             &resolved_svg_path, reporter)) {
    out->swap(resolved_svg_image);
    if (resolved_path && !resolved_svg_path.empty())
      *resolved_path = resolved_svg_path;
    if (reporter) {
      char msg[192];
      snprintf(msg, sizeof(msg),
               "EPUB: inline SVG wrapper resolved id=%u src=%s bytes=%u",
               (unsigned)image_id,
               resolved_svg_path.empty() ? "(unknown)"
                                         : resolved_svg_path.c_str(),
               (unsigned)out->size());
      DBG_LOG(reporter, msg);
    }
    if (close_uf)
      unzClose(uf);
    return true;
  }
  if (close_uf)
    unzClose(uf);
  if (is_svg_wrapper) {
    if (reporter) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper unresolved id=%u %s",
               (unsigned)image_id, image_path.c_str());
      DBG_LOG(reporter, msg);
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
    entry.metadata_ok = false;
    entry.source_width = 0;
    entry.source_height = 0;

    const std::string &image_path = entry.path;
    image_memory::Bytes compressed;
    std::string resolved_path;
    bool need_full_load = false;
    bool already_loaded_full = false;

    if (image_path.compare(0, 4, "fb2:") == 0 ||
        image_path.compare(0, 14, "mobi:recindex:") == 0) {
      // For embedded formats, mark probed immediately (no zip to open).
      entry.metadata_probed = true;
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
      if (!uf) {
        // Zip open failed (likely transient memory pressure).
        // Leave metadata_probed=false so we retry next frame.
        return false;
      }
      // Mark probed only after zip was successfully opened.
      entry.metadata_probed = true;
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

      // Some EPUB images keep SOF markers beyond the small ZIP prefix probe.
      // If lightweight info probing fails, retry once with the full source so
      // layout gets real dimensions instead of falling back to PAGE mode.
      if ((!have_info || info_w <= 0 || info_h <= 0) && !already_loaded_full &&
          image_path.compare(0, 14, "mobi:recindex:") != 0) {
        image_memory::Bytes full_source;
        std::string full_resolved_path;
        if (LoadInlineImageSource(image_id, &full_source, &full_resolved_path) &&
            !full_source.empty()) {
          compressed.swap(full_source);
          have_info = stbi_info_from_memory(compressed.data(),
                                            (int)compressed.size(), &info_w,
                                            &info_h, &info_c) != 0;
          if (!full_resolved_path.empty() &&
              full_resolved_path.compare(0, 5, "data:") != 0) {
            resolved_path = full_resolved_path;
          }
        }
      }

      if ((!have_info || info_w <= 0 || info_h <= 0) &&
          image_path.compare(0, 14, "mobi:recindex:") == 0) {
        // Some MOBI photo records decode fine but fail the lightweight probe.
        // Fall back to a full decode once so pagination still gets dimensions.
        raster_image::Bitmap decoded;
        const char *error=NULL;
        if(raster_image::Decode(compressed.data(),compressed.size(),1,1,0,
                                &decoded,&error)) {
          info_w=decoded.original_width;info_h=decoded.original_height;
          have_info=true;
        }
      }
      if (have_info && info_w > 0 && info_h > 0) {
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
                                 InlineImageLayoutPlan *out,
                                 int author_max_width_override) {
  if (!ts || !out)
    return false;

  InlineImageMetadata meta{};
  EnsureInlineImageMetadata(image_id, &meta);

  InlineImageLayoutRequest req{};
  const InlineImageScreenLayout screen_layout =
      ResolveInlineImageScreenLayoutForOrientation(
          (unsigned char)GetOrientation(), current_screen, ts->margin.bottom,
          ts->GetHeight());
  req.screen_width = screen_layout.current_screen_width;
  req.screen_height = screen_layout.current_screen_height;
  req.next_screen_height = screen_layout.next_screen_height;
  req.margin_left = ts->margin.left;
  req.margin_right = ts->margin.right;
  req.margin_top = ts->margin.top;
  req.margin_bottom = screen_layout.current_margin_bottom;
  req.next_margin_bottom = screen_layout.next_margin_bottom;
  req.line_height = ts->GetHeight();
  req.linespacing = ts->linespacing;
  req.pen_x = pen_x;
  req.pen_y = pen_y;
  req.line_began = line_began;
  req.image_context = image_context;
  req.current_screen = current_screen;
  req.follow_text_lines = GetInlineImageFollowTextLines(image_id);
  req.author_max_width_px =
      (author_max_width_override > 0)
          ? author_max_width_override
          : ((image_id < inline_images.size())
                 ? inline_images[image_id].author_max_width_px
                 : 0);

  *out = ::PlanInlineImageLayout(req, meta);
  return true;
}

bool Book::DrawInlineImage(Text *ts, u16 image_id,
                           const InlineImageLayoutPlan *plan_ptr,
                           int current_screen, u8 align_mode,
                           InlineImageDrawBounds *draw_bounds) {
  if (draw_bounds) *draw_bounds = InlineImageDrawBounds{};
  if (!ts)
    return false;

  InlineImageLayoutPlan local_plan{};
  if (current_screen < 0) {
    const bool left_screen = (ts->GetScreen() == ts->screenleft);
    current_screen = ResolveReadingScreenIndexForPhysicalScreen(
        orientation_utils::IsTurnedRight(GetOrientation()), left_screen);
  }
  if (!plan_ptr) {
    if (!PlanInlineImageLayout(ts, image_id, current_screen, ts->GetPenX(),
                               ts->GetPenY(), ts->linebegan,
                               INLINE_IMAGE_CONTEXT_DEFAULT, &local_plan))
      return false;
    plan_ptr = &local_plan;
  }
  const InlineImageLayoutPlan &plan = *plan_ptr;

  const InlineImageScreenLayout draw_screen_layout =
      ResolveInlineImageScreenLayoutForOrientation(
          (unsigned char)GetOrientation(), current_screen, ts->margin.bottom,
          ts->GetHeight());
  const int screen_w = draw_screen_layout.current_screen_width;
  const int screen_h = draw_screen_layout.current_screen_height;
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
      const InlineImagePagePlacement placement =
          ResolveInlineImagePagePlacement(
              screen_w, screen_h, 0, 0, 0,
              draw_screen_layout.current_margin_bottom, page_imgW, page_imgH, 0,
              0, 0);
      draw_w = placement.draw_width;
      draw_h = placement.draw_height;
      start_x = placement.start_x;
      start_y = placement.start_y;
    }
  }

  if (!need_page_recompute) {
    if (plan.mode == INLINE_IMAGE_LAYOUT_INLINE) {
      start_x = ts->GetPenX();
      start_y = line_top;
    } else {
      if (align_mode == 1) {
        start_x = ts->margin.left;
      } else if (align_mode == 2) {
        start_x = screen_w - ts->margin.right - draw_w;
      } else {
        start_x = ts->margin.left + (text_w - draw_w) / 2;
      }
      start_y = line_top;
    }
  }

  auto blit_pixels = [&](const InlineImageCacheEntry &entry, int dst_x,
                         int dst_y) {
    if (draw_bounds) {
      draw_bounds->x0 = std::max(0, dst_x);
      draw_bounds->y0 = std::max(0, dst_y);
      draw_bounds->x1 = std::min(screen_w, dst_x + entry.width);
      draw_bounds->y1 = std::min(screen_h, dst_y + entry.height);
    }
    u16 *dst = ts->GetScreen();
    const int stride = ts->BufferStride();
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

      const u32 *src_row = entry.pixels.data() + (y * entry.width) + src_x;
      u16 *dst_row = dst + (dy * stride) + draw_x;
      for (int px = 0; px < draw_w_local; px++) {
        u32 argb = src_row[px];
        u8 a = (argb >> 24) & 0xFF;
        u8 r8 = (argb >> 16) & 0xFF;
        u8 g8 = (argb >>  8) & 0xFF;
        u8 b8 =  argb        & 0xFF;
        if (a == 255) {
          dst_row[px] = ((u16)(r8 >> 3) << 11) | ((u16)(g8 >> 2) << 5) | (u16)(b8 >> 3);
        } else if (a > 0) {
          u16 bg = dst_row[px];
          u8 bg_r = ((bg >> 11) & 0x1F) << 3;
          u8 bg_g = ((bg >>  5) & 0x3F) << 2;
          u8 bg_b =  (bg        & 0x1F) << 3;
          u32 ia = 255 - a;
          u8 out_r = (u8)((r8 * a + bg_r * ia + 127) / 255);
          u8 out_g = (u8)((g8 * a + bg_g * ia + 127) / 255);
          u8 out_b = (u8)((b8 * a + bg_b * ia + 127) / 255);
          dst_row[px] = ((u16)(out_r >> 3) << 11) | ((u16)(out_g >> 2) << 5) | (u16)(out_b >> 3);
        }
        // a == 0: fully transparent, skip (leave screen pixel unchanged)
      }
    }
  };

  // LRU cache lookup before image decode; skip when dimensions unknown.
  if (draw_w > 0 && draw_h > 0) {
    // Hash-based O(1) lookup using composite key
    u64 composite_key = ((u64)image_id << 48) | ((u64)(u16)screen_h << 32) |
                        ((u64)(u8)plan.mode << 8) |
                        ((u64)(u16)draw_w & 0xFF);
    auto index_it = inline_image_cache_index.find(composite_key);
    if (index_it != inline_image_cache_index.end()) {
      auto list_it = index_it->second;
      // Verify full match to guard against hash collisions
      if (list_it->image_id == image_id && list_it->screen_h == (u16)screen_h &&
          list_it->layout_mode == (u8)plan.mode &&
          list_it->width == draw_w && list_it->height == draw_h) {
        inline_image_cache.splice(inline_image_cache.begin(),
                                  inline_image_cache, list_it);
        blit_pixels(inline_image_cache.front(), start_x, start_y);
        return true;
      }
    }
  }

  image_memory::Bytes compressed;
  if (!LoadInlineImageSource(image_id, &compressed, NULL) || compressed.empty())
    return false;

  raster_image::ArgbBitmap decoded;
  const char *error=NULL;
  if(!raster_image::DecodeArgb(compressed.data(),compressed.size(),
      draw_w>0 ? draw_w : screen_w,draw_h>0 ? draw_h : screen_h,
      &decoded,&error)) return false;
  if(need_page_recompute && page_imgW==0 && page_imgH==0) {
    const auto placement=ResolveInlineImagePagePlacement(
        screen_w,screen_h,0,0,0,draw_screen_layout.current_margin_bottom,
        decoded.original_width,decoded.original_height,0,0,0);
    draw_w=placement.draw_width;draw_h=placement.draw_height;
    start_x=placement.start_x;start_y=placement.start_y;
  }
  if(draw_w<=0 || draw_h<=0 || (long long)draw_w*draw_h>1500000LL) return false;
  InlineImageCacheEntry entry;
  entry.image_id=image_id;entry.screen_h=(u16)screen_h;entry.bg565=bg565;
  entry.layout_mode=(u8)plan.mode;entry.width=(u16)draw_w;entry.height=(u16)draw_h;
  if(draw_w==decoded.width && draw_h==decoded.height) {
    entry.pixels=std::move(decoded.pixels);
  } else {
    if(!entry.pixels.resize((size_t)draw_w*draw_h,6000000)) return false;
    for(int y=0;y<draw_h;++y) {
      const size_t sy=(size_t)y*decoded.height/draw_h;
      for(int x=0;x<draw_w;++x) {
        const size_t sx=(size_t)x*decoded.width/draw_w;
        entry.pixels.mutable_data()[(size_t)y*draw_w+x]=decoded.pixels[sy*decoded.width+sx];
      }
    }
  }

  const size_t entry_bytes = entry.pixels.size() * sizeof(u32);
  if (entry_bytes <= kInlineImageCacheMaxBytes) {
    while (
        !inline_image_cache.empty() &&
        (inline_image_cache_bytes + entry_bytes > kInlineImageCacheMaxBytes ||
         inline_image_cache.size() >= kInlineImageCacheMaxEntries)) {
      // Remove evicted entry from hash index
      u64 evict_key = ((u64)inline_image_cache.back().image_id << 48) |
                      ((u64)inline_image_cache.back().screen_h << 32) |
                      ((u64)inline_image_cache.back().layout_mode << 8) |
                      ((u64)inline_image_cache.back().width & 0xFF);
      inline_image_cache_index.erase(evict_key);
      inline_image_cache_bytes -=
          inline_image_cache.back().pixels.size() * sizeof(u32);
      inline_image_cache.pop_back();
    }
    inline_image_cache.push_front(std::move(entry));
    inline_image_cache_bytes += entry_bytes;
    // Insert new entry into hash index
    u64 new_key = ((u64)inline_image_cache.front().image_id << 48) |
                  ((u64)inline_image_cache.front().screen_h << 32) |
                  ((u64)inline_image_cache.front().layout_mode << 8) |
                  ((u64)inline_image_cache.front().width & 0xFF);
    inline_image_cache_index[new_key] = inline_image_cache.begin();
    blit_pixels(inline_image_cache.front(), start_x, start_y);
  } else {
    blit_pixels(entry, start_x, start_y);
  }

  return true;
}
