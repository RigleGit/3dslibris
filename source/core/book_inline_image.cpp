#include "book.h"

#include "stb_image.h"
#include "unzip.h"
#include <string.h>
#include <utility>

namespace {
static const size_t kInlineImageCacheMaxBytes = 1024 * 1024;
}

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
  ClearInlineImageCache();
}

bool Book::DrawInlineImage(Text *ts, u16 image_id) {
  if (!ts)
    return false;
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

  for (std::list<InlineImageCacheEntry>::iterator it = inline_image_cache.begin();
       it != inline_image_cache.end(); ++it) {
    if (it->image_id == image_id && it->screen_h == (u16)screen_h &&
        it->bg565 == bg565) {
      inline_image_cache.splice(inline_image_cache.begin(), inline_image_cache,
                                it);
      blit_cached(inline_image_cache.front());
      return true;
    }
  }

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
      fi.uncompressed_size == 0 || fi.uncompressed_size > (4 * 1024 * 1024)) {
    unzCloseCurrentFile(uf);
    unzClose(uf);
    return false;
  }

  std::vector<u8> compressed(fi.uncompressed_size);
  int bytes_read = unzReadCurrentFile(uf, compressed.data(), fi.uncompressed_size);
  unzCloseCurrentFile(uf);
  unzClose(uf);
  if (bytes_read <= 0)
    return false;

  int info_w = 0, info_h = 0, info_c = 0;
  if (!stbi_info_from_memory(compressed.data(), bytes_read, &info_w, &info_h,
                             &info_c))
    return false;
  if (info_w <= 0 || info_h <= 0)
    return false;
  if ((long long)info_w * (long long)info_h > 1500000LL)
    return false;

  int imgW = 0, imgH = 0, channels = 0;
  unsigned char *pixels = stbi_load_from_memory(
      compressed.data(), bytes_read, &imgW, &imgH, &channels, 4);
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
