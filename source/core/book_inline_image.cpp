/*
    3dslibris - book_inline_image.cpp
    New module for the 3DS port by Rigle.

    Summary:
    - Decodes inline images referenced by EPUB/FB2 content.
    - Maintains bounded caches for decoded/scaled image payloads.
    - Provides safe limits for image bytes/dimensions on 3DS memory budget.
*/

#include "book.h"

#include "base64_utils.h"
#include "path_utils.h"
#include "stb_image.h"
#include "string_utils.h"
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
static const size_t kSvgWrapperMaxBytes = 512 * 1024;

static bool StartsWithNoCase(const std::string &s, const char *prefix) {
  if (!prefix)
    return false;
  size_t len = strlen(prefix);
  if (s.size() < len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char a = (unsigned char)s[i];
    unsigned char b = (unsigned char)prefix[i];
    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

static bool DecodeDataUriImage(const std::string &href, std::vector<u8> *out,
                               App *app = NULL) {
  if (!out || !StartsWithNoCase(href, "data:"))
    return false;
  size_t comma = href.find(',');
  if (comma == std::string::npos || comma <= 5 || comma + 1 >= href.size())
    return false;

  std::string meta = ToLowerAscii(href.substr(5, comma - 5));
  if (meta.find("image/") == std::string::npos ||
      meta.find(";base64") == std::string::npos) {
    return false;
  }

  std::string payload = href.substr(comma + 1);
  bool ok = DecodeBase64Bytes(payload, out, kEpubInlineImageMaxBytes);
  if (app) {
    char msg[128];
    snprintf(msg, sizeof(msg), "EPUB: inline SVG data-uri decode %s bytes=%u",
             ok ? "ok" : "fail", (unsigned)out->size());
    app->PrintStatus(msg);
  }
  return ok;
}

static bool ReadZipEntryBinary(unzFile uf, const std::string &path,
                               std::vector<u8> *out, size_t max_bytes) {
  if (!out || !uf || path.empty())
    return false;
  out->clear();

  if (unzLocateFile(uf, path.c_str(), 2) != UNZ_OK)
    return false;
  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
      fi.uncompressed_size == 0 || fi.uncompressed_size > max_bytes ||
      fi.uncompressed_size > (uLong)INT_MAX) {
    unzCloseCurrentFile(uf);
    return false;
  }

  out->resize((size_t)fi.uncompressed_size);
  int total = 0;
  while (total < (int)out->size()) {
    int n = unzReadCurrentFile(uf, out->data() + total,
                               (unsigned int)(out->size() - (size_t)total));
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
  if (buf.empty())
    return false;
  std::string lower_path = ToLowerAscii(path_hint);
  if (lower_path.size() >= 4 &&
      lower_path.rfind(".svg") == lower_path.size() - 4) {
    return true;
  }
  size_t sample = std::min((size_t)512, buf.size());
  std::string head((const char *)buf.data(), sample);
  return ToLowerAscii(head).find("<svg") != std::string::npos;
}

static bool
ResolveSvgWrapperImage(const std::string &epubpath, const std::string &svg_path,
                       const std::vector<u8> &svg_buf, std::vector<u8> *out,
                       std::string *resolved_path = NULL, App *app = NULL) {
  if (!out || svg_buf.empty() || svg_buf.size() > kSvgWrapperMaxBytes) {
    if (app)
      app->PrintStatus("EPUB: inline SVG wrapper skip (size/empty)");
    return false;
  }
  out->clear();
  if (resolved_path)
    resolved_path->clear();

  std::string xml((const char *)svg_buf.data(), svg_buf.size());
  unzFile uf = unzOpen(epubpath.c_str());
  if (!uf)
    return false;

  int image_tags_seen = 0;
  int href_attempts = 0;
  size_t pos = 0;
  while (pos < xml.size()) {
    size_t lt = xml.find('<', pos);
    if (lt == std::string::npos)
      break;
    size_t gt = xml.find('>', lt + 1);
    if (gt == std::string::npos)
      break;
    pos = gt + 1;

    std::string tag = Trim(xml.substr(lt + 1, gt - lt - 1));
    if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?')
      continue;

    size_t name_end = 0;
    while (name_end < tag.size() &&
           IsHtmlNameChar((unsigned char)tag[name_end]))
      name_end++;
    std::string name = ToLowerAscii(tag.substr(0, name_end));
    if (name != "image" && name != "img")
      continue;
    image_tags_seen++;

    std::string href = ExtractHtmlAttrValue(tag, "href");
    if (href.empty())
      href = ExtractHtmlAttrValue(tag, "xlink:href");
    if (href.empty())
      href = ExtractHtmlAttrValue(tag, "src");
    href = Trim(href);
    if (href.empty())
      continue;
    href_attempts++;

    if (StartsWithNoCase(href, "data:")) {
      if (DecodeDataUriImage(href, out, app)) {
        if (resolved_path)
          *resolved_path = "data:image";
        unzClose(uf);
        return true;
      }
      continue;
    }

    std::string resolved =
        StripFragmentAndQuery(ResolveRelativePath(svg_path, href));
    if (resolved.empty() || resolved == svg_path)
      continue;
    if (ReadZipEntryBinary(uf, resolved, out, kEpubInlineImageMaxBytes)) {
      if (resolved_path)
        *resolved_path = resolved;
      if (app) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "EPUB: inline SVG wrapper href resolved %s bytes=%u",
                 resolved.c_str(), (unsigned)out->size());
        app->PrintStatus(msg);
      }
      unzClose(uf);
      return true;
    }
  }

  if (app) {
    char msg[192];
    snprintf(msg, sizeof(msg),
             "EPUB: inline SVG wrapper unresolved tags=%d hrefs=%d path=%s",
             image_tags_seen, href_attempts, svg_path.c_str());
    app->PrintStatus(msg);
  }
  unzClose(uf);
  out->clear();
  return false;
}
} // namespace

u16 Book::RegisterInlineImage(const std::string &path) {
  if (path.empty())
    return 0;
  for (u16 i = 0; i < inline_images.size(); i++) {
    if (inline_images[i] == path)
      return i;
  }
  if (inline_images.size() >= 65535)
    return 0;
  inline_images.push_back(path);
  return (u16)(inline_images.size() - 1);
}

const std::string *Book::GetInlineImagePath(u16 id) const {
  if (id >= inline_images.size())
    return NULL;
  return &inline_images[id];
}

void Book::ClearInlineImageCache() {
  inline_image_cache.clear();
  inline_image_cache_bytes = 0;
}

void Book::ClearInlineImages() {
  inline_images.clear();
  fb2_inline_images.clear();
  fb2_inline_images_bytes = 0;
  ClearInlineImageCache();
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

bool Book::DrawInlineImage(Text *ts, u16 image_id) {
  if (!ts)
    return false;
  App *app = GetApp();
  const std::string *image_path = GetInlineImagePath(image_id);
  if (!image_path || image_path->empty())
    return false;

  const bool left_screen = (ts->GetScreen() == ts->screenleft);
  const int screen_w = 240;
  const int screen_h = left_screen ? 400 : 320;
  const int pad = 2;
  const int avail_w = screen_w - (pad * 2);
  const int avail_h = screen_h - (pad * 2);
  u16 bg565 = ts->GetBgColor();

  auto blit_cached = [&](const InlineImageCacheEntry &entry) {
    u16 *dst = ts->GetScreen();
    const int stride = ts->display.height;
    for (int y = 0; y < entry.height; y++) {
      int dy = entry.start_y + y;
      if (dy < 0 || dy >= screen_h)
        continue;

      int draw_x = entry.start_x;
      int draw_w = entry.width;
      int src_x = 0;
      if (draw_x < 0) {
        src_x = -draw_x;
        draw_w -= src_x;
        draw_x = 0;
      }
      if (draw_x + draw_w > screen_w)
        draw_w = screen_w - draw_x;
      if (draw_w <= 0)
        continue;

      const u16 *src_row = entry.pixels.data() + (y * entry.width) + src_x;
      u16 *dst_row = dst + (dy * stride) + draw_x;
      memcpy(dst_row, src_row, draw_w * sizeof(u16));
    }
  };

  for (std::list<InlineImageCacheEntry>::iterator it =
           inline_image_cache.begin();
       it != inline_image_cache.end(); ++it) {
    if (it->image_id == image_id && it->screen_h == (u16)screen_h &&
        it->bg565 == bg565) {
      inline_image_cache.splice(inline_image_cache.begin(), inline_image_cache,
                                it);
      blit_cached(inline_image_cache.front());
      return true;
    }
  }

  const u8 *compressed_data = NULL;
  int compressed_size = 0;
  std::vector<u8> compressed;
  if (image_path->compare(0, 4, "fb2:") == 0) {
    std::string key = image_path->substr(4);
    if (!key.empty() && key[0] == '#')
      key.erase(0, 1);
    std::unordered_map<std::string, std::vector<u8>>::const_iterator hit =
        fb2_inline_images.find(key);
    if (hit == fb2_inline_images.end() || hit->second.empty())
      return false;
    compressed_data = hit->second.data();
    compressed_size = (int)hit->second.size();
  } else {
    std::string epubpath = foldername + "/" + filename;
    unzFile uf = unzOpen(epubpath.c_str());
    if (!uf)
      return false;

    int rc = unzLocateFile(uf, image_path->c_str(), 2);
    if (rc != UNZ_OK) {
      unzClose(uf);
      return false;
    }
    if (unzOpenCurrentFile(uf) != UNZ_OK) {
      unzClose(uf);
      return false;
    }

    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK ||
        fi.uncompressed_size == 0 ||
        fi.uncompressed_size > kEpubInlineImageMaxBytes) {
      unzCloseCurrentFile(uf);
      unzClose(uf);
      return false;
    }

    compressed.resize(fi.uncompressed_size);
    int bytes_read =
        unzReadCurrentFile(uf, compressed.data(), fi.uncompressed_size);
    unzCloseCurrentFile(uf);
    unzClose(uf);
    if (bytes_read <= 0)
      return false;

    compressed_data = compressed.data();
    compressed_size = bytes_read;
  }

  int info_w = 0, info_h = 0, info_c = 0;
  bool has_info = stbi_info_from_memory(compressed_data, compressed_size,
                                        &info_w, &info_h, &info_c) != 0;
  if (!has_info && image_path->compare(0, 4, "fb2:") != 0) {
    bool is_svg_wrapper = LooksLikeSvgWrapper(*image_path, compressed);
    std::vector<u8> resolved_svg_image;
    std::string resolved_svg_path;
    if (is_svg_wrapper && app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper detected id=%u %s",
               (unsigned)image_id, image_path->c_str());
      app->PrintStatus(msg);
    }
    if (is_svg_wrapper &&
        ResolveSvgWrapperImage(foldername + "/" + filename, *image_path,
                               compressed, &resolved_svg_image,
                               &resolved_svg_path, app)) {
      compressed.swap(resolved_svg_image);
      compressed_data = compressed.data();
      compressed_size = (int)compressed.size();
      has_info = stbi_info_from_memory(compressed_data, compressed_size,
                                       &info_w, &info_h, &info_c) != 0;
      if (app) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "EPUB: inline SVG wrapper resolved id=%u src=%s bytes=%u",
                 (unsigned)image_id,
                 resolved_svg_path.empty() ? "(unknown)"
                                           : resolved_svg_path.c_str(),
                 (unsigned)compressed.size());
        app->PrintStatus(msg);
      }
    } else if (!has_info && LooksLikeSvgWrapper(*image_path, compressed) &&
               app) {
      char msg[192];
      snprintf(msg, sizeof(msg), "EPUB: inline SVG wrapper unresolved id=%u %s",
               (unsigned)image_id, image_path->c_str());
      app->PrintStatus(msg);
    }
  }
  if (!has_info)
    return false;
  if (info_w <= 0 || info_h <= 0)
    return false;
  if ((long long)info_w * (long long)info_h > 1500000LL)
    return false;

  int imgW = 0, imgH = 0, channels = 0;
  unsigned char *pixels = stbi_load_from_memory(
      compressed_data, compressed_size, &imgW, &imgH, &channels, 4);
  if (!pixels)
    return false;

  float sx = (float)avail_w / (float)imgW;
  float sy = (float)avail_h / (float)imgH;
  float scale = (sx < sy) ? sx : sy;
  if (scale > 1.0f)
    scale = 1.0f;

  int draw_w = (int)(imgW * scale);
  int draw_h = (int)(imgH * scale);
  if (draw_w < 1)
    draw_w = 1;
  if (draw_h < 1)
    draw_h = 1;

  int start_x = pad + (avail_w - draw_w) / 2;
  int start_y = pad + (avail_h - draw_h) / 2;
  if (start_x < 0)
    start_x = 0;
  if (start_y < 0)
    start_y = 0;

  InlineImageCacheEntry entry;
  entry.image_id = image_id;
  entry.screen_h = (u16)screen_h;
  entry.bg565 = bg565;
  entry.start_x = (u16)start_x;
  entry.start_y = (u16)start_y;
  entry.width = (u16)draw_w;
  entry.height = (u16)draw_h;
  entry.pixels.resize(draw_w * draw_h);

  u8 bg_r5 = (bg565 >> 11) & 0x1F;
  u8 bg_g6 = (bg565 >> 5) & 0x3F;
  u8 bg_b5 = bg565 & 0x1F;
  u8 bg_r8 = (bg_r5 << 3) | (bg_r5 >> 2);
  u8 bg_g8 = (bg_g6 << 2) | (bg_g6 >> 4);
  u8 bg_b8 = (bg_b5 << 3) | (bg_b5 >> 2);

  for (int y = 0; y < draw_h; y++) {
    int src_y = (int)(y / scale);
    if (src_y >= imgH)
      src_y = imgH - 1;
    for (int x = 0; x < draw_w; x++) {
      int src_x = (int)(x / scale);
      if (src_x >= imgW)
        src_x = imgW - 1;
      unsigned char *px = &pixels[(src_y * imgW + src_x) * 4];

      u8 r8 = px[0];
      u8 g8 = px[1];
      u8 b8 = px[2];
      u8 a8 = px[3];
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
    blit_cached(inline_image_cache.front());
  } else {
    // Should never happen with 240x400 cap, but keep a direct fallback path.
    blit_cached(entry);
  }

  return true;
}
