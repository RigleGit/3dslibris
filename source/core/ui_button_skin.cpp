/*
    3dslibris - ui_button_skin.cpp
    New module for Nintendo 3DS port by Rigle.

    Summary:
    - Procedural button skin generator (RGB565 + alpha mask).
    - Small fixed-size cache keyed by width/height/state/icon-presence.
    - Runtime PNG icon loading with vector fallback icon rasterization.
*/

#include "ui_button_skin.h"

#include <algorithm>
#include <cmath>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "stb_image.h"

namespace {

static const int kCacheMax = 48;

struct IconBitmap {
  bool loaded;
  int width;
  int height;
  std::vector<u8> rgba;
  IconBitmap() : loaded(false), width(0), height(0) {}
};

struct ButtonCacheKey {
  u16 w;
  u16 h;
  u8 state;
  u8 with_icon;
};

struct ButtonCacheEntry {
  bool used;
  u32 age;
  ButtonCacheKey key;
  std::vector<u16> rgb565;
  std::vector<u8> alpha;
  ButtonCacheEntry() : used(false), age(0), key{0, 0, 0, 0} {}
};

static bool g_inited = false;
static u32 g_age = 1;
static ButtonCacheEntry g_cache[kCacheMax];
static IconBitmap g_icons[UI_BUTTON_ICON_COUNT];

static inline float clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static inline float smoothstepf(float a, float b, float x) {
  float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static inline u16 RGB565FromU8(float r, float g, float b) {
  const int ri = (int)clampf(r, 0.0f, 255.0f);
  const int gi = (int)clampf(g, 0.0f, 255.0f);
  const int bi = (int)clampf(b, 0.0f, 255.0f);
  return (u16)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

static inline void RGB565ToU8(u16 c, int *r, int *g, int *b) {
  int r5 = (c >> 11) & 0x1F;
  int g6 = (c >> 5) & 0x3F;
  int b5 = c & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

static inline u16 Blend565(u16 fg, u16 bg, u8 alpha) {
  if (alpha == 255)
    return fg;
  if (alpha == 0)
    return bg;
  int fr, fgc, fb, br, bgc, bb;
  RGB565ToU8(fg, &fr, &fgc, &fb);
  RGB565ToU8(bg, &br, &bgc, &bb);
  int a = (int)alpha;
  int ia = 255 - a;
  int r = (fr * a + br * ia + 127) / 255;
  int g = (fgc * a + bgc * ia + 127) / 255;
  int b = (fb * a + bb * ia + 127) / 255;
  return RGB565FromU8((float)r, (float)g, (float)b);
}

static inline float sd_round_rect(float px, float py, float cx, float cy,
                                  float hw, float hh, float r) {
  float dx = fabsf(px - cx) - (hw - r);
  float dy = fabsf(py - cy) - (hh - r);
  float ax = fmaxf(dx, 0.0f);
  float ay = fmaxf(dy, 0.0f);
  return sqrtf(ax * ax + ay * ay) + fminf(fmaxf(dx, dy), 0.0f) - r;
}

static bool read_file_all(const char *path, std::vector<u8> *out) {
  if (!path || !out)
    return false;
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(fp);
    return false;
  }
  out->resize((size_t)sz);
  bool ok = fread(out->data(), 1, (size_t)sz, fp) == (size_t)sz;
  fclose(fp);
  if (!ok) {
    out->clear();
    return false;
  }
  return true;
}

static bool load_icon_png(IconBitmap *icon, const char *path) {
  if (!icon || !path)
    return false;
  std::vector<u8> encoded;
  if (!read_file_all(path, &encoded))
    return false;
  int w = 0, h = 0, n = 0;
  unsigned char *decoded =
      stbi_load_from_memory(encoded.data(), (int)encoded.size(), &w, &h, &n, 4);
  if (!decoded || w <= 0 || h <= 0) {
    if (decoded)
      stbi_image_free(decoded);
    return false;
  }
  icon->rgba.assign(decoded, decoded + (size_t)w * (size_t)h * 4);
  icon->width = w;
  icon->height = h;
  icon->loaded = true;
  stbi_image_free(decoded);
  return true;
}

static void try_load_icons_once() {
  if (!g_inited)
    return;
  bool have_any = false;
  for (int i = 0; i < UI_BUTTON_ICON_COUNT; i++) {
    if (g_icons[i].loaded) {
      have_any = true;
      break;
    }
  }
  if (have_any)
    return;

  struct IconSpec {
    UiButtonIconId id;
    const char *filename;
  } specs[] = {
      {UI_BUTTON_ICON_GEAR, "gear.png"},
      {UI_BUTTON_ICON_NEXT, "next.png"},
      {UI_BUTTON_ICON_PREV, "prev.png"},
      {UI_BUTTON_ICON_BACK, "back.png"},
      {UI_BUTTON_ICON_HOME, "home.png"},
  };

  const char *dirs[] = {
      "sdmc:/3ds/3dslibris/resources/ui/icons/png",
      "sdmc:/3ds/3dslibris/resources/ui/icons",
      "resources/ui/icons/png",
      "resources/ui/icons",
      "sdmc:/3ds/3dslibris/resources",
      "data/ui/icons/png",
      "./data/ui/icons/png",
      nullptr,
  };

  char path[512];
  for (int d = 0; dirs[d]; d++) {
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
      IconBitmap &icon = g_icons[(int)specs[i].id];
      if (icon.loaded)
        continue;
      snprintf(path, sizeof(path), "%s/%s", dirs[d], specs[i].filename);
      load_icon_png(&icon, path);
    }
  }
}

static void cache_reset_internal() {
  for (int i = 0; i < kCacheMax; i++) {
    g_cache[i].used = false;
    g_cache[i].age = 0;
    g_cache[i].rgb565.clear();
    g_cache[i].alpha.clear();
  }
  g_age = 1;
}

static bool key_equals(const ButtonCacheKey &a, const ButtonCacheKey &b) {
  return a.w == b.w && a.h == b.h && a.state == b.state &&
         a.with_icon == b.with_icon;
}

static ButtonCacheEntry *cache_find(const ButtonCacheKey &key) {
  for (int i = 0; i < kCacheMax; i++) {
    if (g_cache[i].used && key_equals(g_cache[i].key, key))
      return &g_cache[i];
  }
  return nullptr;
}

static ButtonCacheEntry *cache_alloc(const ButtonCacheKey &key) {
  ButtonCacheEntry *slot = nullptr;
  u32 oldest_age = 0xFFFFFFFFu;
  // Reuse a free slot first; otherwise evict least-recently-used entry.
  for (int i = 0; i < kCacheMax; i++) {
    if (!g_cache[i].used) {
      slot = &g_cache[i];
      break;
    }
    if (g_cache[i].age < oldest_age) {
      oldest_age = g_cache[i].age;
      slot = &g_cache[i];
    }
  }
  if (!slot)
    return nullptr;
  slot->used = true;
  slot->age = g_age++;
  slot->key = key;
  size_t count = (size_t)key.w * (size_t)key.h;
  slot->rgb565.assign(count, 0);
  slot->alpha.assign(count, 0);
  return slot;
}

static void put_layer(float *r, float *g, float *b, float *a, float sr, float sg,
                      float sb, float sa) {
  float outA = sa + (*a) * (1.0f - sa);
  if (outA <= 0.0001f) {
    *r = *g = *b = *a = 0.0f;
    return;
  }
  float outR = (sr * sa + (*r) * (*a) * (1.0f - sa)) / outA;
  float outG = (sg * sa + (*g) * (*a) * (1.0f - sa)) / outA;
  float outB = (sb * sa + (*b) * (*a) * (1.0f - sa)) / outA;
  *r = outR;
  *g = outG;
  *b = outB;
  *a = outA;
}

static void generate_button_bitmap(ButtonCacheEntry *entry) {
  const int w = (int)entry->key.w;
  const int h = (int)entry->key.h;
  const bool with_icon = entry->key.with_icon != 0;
  UiButtonSkinState state = (UiButtonSkinState)entry->key.state;

  float fillTopR = 241.0f, fillTopG = 231.0f, fillTopB = 213.0f;
  float fillBotR = 229.0f, fillBotG = 216.0f, fillBotB = 196.0f;
  float borderOuterR = 114.0f, borderOuterG = 80.0f, borderOuterB = 48.0f;
  float borderInnerR = 186.0f, borderInnerG = 158.0f, borderInnerB = 124.0f;
  float shadowR = 78.0f, shadowG = 56.0f, shadowB = 36.0f;

  if (state == UI_BUTTON_STATE_PRESSED) {
    fillTopR -= 9.0f;
    fillTopG -= 8.0f;
    fillTopB -= 7.0f;
    fillBotR -= 10.0f;
    fillBotG -= 9.0f;
    fillBotB -= 8.0f;
  } else if (state == UI_BUTTON_STATE_DISABLED) {
    fillTopR = lerpf(fillTopR, 214.0f, 0.62f);
    fillTopG = lerpf(fillTopG, 214.0f, 0.62f);
    fillTopB = lerpf(fillTopB, 214.0f, 0.62f);
    fillBotR = lerpf(fillBotR, 206.0f, 0.62f);
    fillBotG = lerpf(fillBotG, 206.0f, 0.62f);
    fillBotB = lerpf(fillBotB, 206.0f, 0.62f);
  }

  float cx = (float)w * 0.5f;
  float cy = (float)h * 0.5f;
  float hw = (float)w * 0.5f - 1.6f;
  float hh = (float)h * 0.5f - 1.8f;
  float radius = clampf((float)h * 0.30f, 4.0f, 12.0f);

  float shadowY = (state == UI_BUTTON_STATE_PRESSED) ? 1.0f : 2.0f;
  float shadowAlphaMul = (state == UI_BUTTON_STATE_PRESSED) ? 0.13f : 0.26f;

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float dr = 0.0f, dg = 0.0f, db = 0.0f, da = 0.0f;

      float px = (float)x + 0.5f;
      float py = (float)y + 0.5f;

      float sdShadow = sd_round_rect(px, py, cx, cy + shadowY, hw, hh, radius);
      float shadowMask = 1.0f - smoothstepf(0.0f, 4.0f, sdShadow);
      shadowMask *= shadowAlphaMul;
      if (shadowMask > 0.001f) {
        put_layer(&dr, &dg, &db, &da, shadowR / 255.0f, shadowG / 255.0f,
                  shadowB / 255.0f, shadowMask);
      }

      float sd = sd_round_rect(px, py, cx, cy, hw, hh, radius);
      float shapeA = 1.0f - smoothstepf(0.0f, 1.2f, sd);

      if (shapeA > 0.001f) {
        float ty = (h > 1) ? ((float)y / (float)(h - 1)) : 0.0f;
        float fr = lerpf(fillTopR, fillBotR, ty);
        float fg = lerpf(fillTopG, fillBotG, ty);
        float fb = lerpf(fillTopB, fillBotB, ty);

        float nx = (w > 1) ? (((float)x - (float)(w - 1) * 0.5f) /
                              ((float)(w - 1) * 0.5f))
                           : 0.0f;
        float vignette = 1.0f - 0.05f * powf(fabsf(nx), 1.8f);
        fr *= vignette;
        fg *= vignette;
        fb *= vignette;

        // Border mask near the edge only (do not darken full interior).
        float insideDist = -sd;
        float outerBand = 1.0f - smoothstepf(1.4f, 3.0f, insideDist);
        fr = lerpf(fr, borderOuterR, outerBand);
        fg = lerpf(fg, borderOuterG, outerBand);
        fb = lerpf(fb, borderOuterB, outerBand);

        float innerBand =
            smoothstepf(-3.0f, -2.0f, sd) * (1.0f - smoothstepf(-1.3f, -0.4f, sd));
        fr = lerpf(fr, borderInnerR, innerBand * 0.58f);
        fg = lerpf(fg, borderInnerG, innerBand * 0.58f);
        fb = lerpf(fb, borderInnerB, innerBand * 0.58f);

        float topBand = (1.0f - ty);
        float high = smoothstepf(-3.4f, -1.0f, sd) * topBand * 0.20f;
        fr = lerpf(fr, 254.0f, high);
        fg = lerpf(fg, 251.0f, high);
        fb = lerpf(fb, 244.0f, high);

        if (with_icon) {
          int iconW = UiButtonSkin_IconBlockWidth(h);
          float splitX = (float)(w - iconW - 4);
          if ((float)x > splitX) {
            float t = clampf(((float)x - splitX) / (float)iconW, 0.0f, 1.0f);
            fr = lerpf(fr, fr - 7.0f, 0.18f + t * 0.12f);
            fg = lerpf(fg, fg - 6.0f, 0.18f + t * 0.12f);
            fb = lerpf(fb, fb - 5.0f, 0.18f + t * 0.12f);
            if (x == (int)splitX || x == (int)splitX + 1) {
              fr = lerpf(fr, borderInnerR, 0.45f);
              fg = lerpf(fg, borderInnerG, 0.45f);
              fb = lerpf(fb, borderInnerB, 0.45f);
            }
          }
        }

        if (state == UI_BUTTON_STATE_SELECTED) {
          float sel =
              smoothstepf(-4.0f, -1.4f, sd) * (1.0f - smoothstepf(-1.1f, -0.1f, sd));
          fr = lerpf(fr, 250.0f, sel * 0.18f);
          fg = lerpf(fg, 236.0f, sel * 0.18f);
          fb = lerpf(fb, 192.0f, sel * 0.18f);
        }

        float alpha = (state == UI_BUTTON_STATE_DISABLED) ? shapeA * 0.65f : shapeA;
        put_layer(&dr, &dg, &db, &da, fr / 255.0f, fg / 255.0f, fb / 255.0f,
                  alpha);
      }

      size_t idx = (size_t)y * (size_t)w + (size_t)x;
      entry->rgb565[idx] = RGB565FromU8(dr * 255.0f, dg * 255.0f, db * 255.0f);
      entry->alpha[idx] = (u8)clampf(da * 255.0f, 0.0f, 255.0f);
    }
  }
}

static ButtonCacheEntry *get_or_build_cache(int w, int h, UiButtonSkinState state,
                                            bool with_icon) {
  if (w <= 2 || h <= 2)
    return nullptr;

  ButtonCacheKey key;
  key.w = (u16)w;
  key.h = (u16)h;
  key.state = (u8)state;
  key.with_icon = with_icon ? 1 : 0;

  // Cache hit: avoid regenerating gradients/borders every frame.
  ButtonCacheEntry *entry = cache_find(key);
  if (entry) {
    entry->age = g_age++;
    return entry;
  }

  entry = cache_alloc(key);
  if (!entry)
    return nullptr;

  // Cache miss: generate once, then reuse while key stays unchanged.
  generate_button_bitmap(entry);
  return entry;
}

static void fill_rect_clip(u16 *screen, int stride, int logicalHeight, int x0,
                           int y0, int x1, int y1, u16 color, u8 alpha) {
  if (!screen || stride <= 0 || alpha == 0)
    return;
  if (x0 > x1)
    std::swap(x0, x1);
  if (y0 > y1)
    std::swap(y0, y1);
  if (x1 <= 0 || y1 <= 0 || x0 >= 240 || y0 >= logicalHeight)
    return;
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(240, x1);
  y1 = std::min(logicalHeight, y1);
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      size_t didx = (size_t)y * (size_t)stride + (size_t)x;
      screen[didx] = Blend565(color, screen[didx], alpha);
    }
  }
}

static void draw_icon_fallback(u16 *screen, int stride, int logicalHeight, int x,
                               int y, int w, int h, UiButtonIconId icon,
                               bool disabled) {
  int iconBlock = UiButtonSkin_IconBlockWidth(h);
  int boxSize = iconBlock - 8;
  if (boxSize < 8)
    boxSize = 8;
  int ox = x + w - iconBlock + (iconBlock - boxSize) / 2;
  int oy = y + (h - boxSize) / 2;

  const u16 ink = RGB565FromU8(110.0f, 73.0f, 42.0f);
  const u8 alpha = disabled ? 160 : 230;

  if (icon == UI_BUTTON_ICON_NEXT || icon == UI_BUTTON_ICON_PREV ||
      icon == UI_BUTTON_ICON_BACK) {
    int mid = oy + boxSize / 2;
    int stem = std::max(2, boxSize / 7);
    int shaft0 = ox + boxSize / 5;
    int shaft1 = ox + (boxSize * 3) / 5;
    if (icon == UI_BUTTON_ICON_BACK || icon == UI_BUTTON_ICON_PREV) {
      shaft0 = ox + (boxSize * 2) / 5;
      shaft1 = ox + (boxSize * 4) / 5;
    }
    fill_rect_clip(screen, stride, logicalHeight, shaft0, mid - stem, shaft1,
                   mid + stem + 1, ink, alpha);
    for (int i = 0; i < boxSize / 2; i++) {
      int y0 = mid - i;
      int y1 = mid + i + 1;
      if (icon == UI_BUTTON_ICON_NEXT) {
        int x0 = ox + (boxSize * 2) / 5 + i / 2;
        int x1 = ox + boxSize - 1 - i / 3;
        fill_rect_clip(screen, stride, logicalHeight, x0, y0, x1, y1, ink,
                       alpha);
      } else {
        int x0 = ox + i / 3;
        int x1 = ox + (boxSize * 3) / 5 - i / 2;
        fill_rect_clip(screen, stride, logicalHeight, x0, y0, x1, y1, ink,
                       alpha);
      }
    }
    return;
  }

  if (icon == UI_BUTTON_ICON_HOME) {
    int bw = (boxSize * 2) / 3;
    int bh = boxSize / 2;
    int bx = ox + (boxSize - bw) / 2;
    int by = oy + boxSize / 2;
    fill_rect_clip(screen, stride, logicalHeight, bx, by, bx + bw, by + bh,
                   ink, alpha);
    int roofH = boxSize / 2;
    int cx = ox + boxSize / 2;
    for (int i = 0; i < roofH; i++) {
      int hw = (roofH - i);
      fill_rect_clip(screen, stride, logicalHeight, cx - hw, by - i - 1,
                     cx + hw + 1, by - i, ink, alpha);
    }
    int doorW = std::max(2, bw / 4);
    fill_rect_clip(screen, stride, logicalHeight, cx - doorW / 2, by + bh / 2,
                   cx + doorW / 2 + 1, by + bh, RGB565FromU8(228, 212, 187),
                   alpha);
    return;
  }

  if (icon == UI_BUTTON_ICON_GEAR) {
    int cx = ox + boxSize / 2;
    int cy = oy + boxSize / 2;
    int rOut = std::max(4, boxSize / 3);
    int rIn = std::max(2, rOut / 2);

    for (int yy = oy; yy < oy + boxSize; yy++) {
      for (int xx = ox; xx < ox + boxSize; xx++) {
        int dx = xx - cx;
        int dy = yy - cy;
        int d2 = dx * dx + dy * dy;
        if (d2 <= rOut * rOut && d2 >= rIn * rIn) {
          fill_rect_clip(screen, stride, logicalHeight, xx, yy, xx + 1, yy + 1,
                         ink, alpha);
        }
      }
    }
    int tooth = std::max(1, boxSize / 8);
    fill_rect_clip(screen, stride, logicalHeight, cx - tooth, oy, cx + tooth + 1,
                   oy + tooth + 2, ink, alpha);
    fill_rect_clip(screen, stride, logicalHeight, cx - tooth, oy + boxSize - tooth - 2,
                   cx + tooth + 1, oy + boxSize, ink, alpha);
    fill_rect_clip(screen, stride, logicalHeight, ox, cy - tooth, ox + tooth + 2,
                   cy + tooth + 1, ink, alpha);
    fill_rect_clip(screen, stride, logicalHeight, ox + boxSize - tooth - 2, cy - tooth,
                   ox + boxSize, cy + tooth + 1, ink, alpha);
  }
}

} // namespace

bool UiButtonSkin_Init() {
  if (g_inited)
    return true;
  g_inited = true;
  cache_reset_internal();
  try_load_icons_once();
  return true;
}

void UiButtonSkin_Exit() {
  if (!g_inited)
    return;
  cache_reset_internal();
  for (int i = 0; i < UI_BUTTON_ICON_COUNT; i++) {
    g_icons[i].loaded = false;
    g_icons[i].width = 0;
    g_icons[i].height = 0;
    g_icons[i].rgba.clear();
  }
  g_inited = false;
}

void UiButtonSkin_ResetCache() { cache_reset_internal(); }

UiButtonIconId UiButtonSkin_IconFromLabel(const char *label) {
  if (!label || !*label)
    return UI_BUTTON_ICON_NONE;

  while (*label && isspace((unsigned char)*label))
    label++;

  char lower[64];
  size_t n = strlen(label);
  while (n > 0 && isspace((unsigned char)label[n - 1]))
    n--;
  if (n > sizeof(lower) - 1)
    n = sizeof(lower) - 1;
  for (size_t i = 0; i < n; i++)
    lower[i] = (char)tolower((unsigned char)label[i]);
  lower[n] = '\0';

  if (strcmp(lower, "settings") == 0 || strcmp(lower, "setting") == 0)
    return UI_BUTTON_ICON_GEAR;
  if (strcmp(lower, "next") == 0)
    return UI_BUTTON_ICON_NEXT;
  if (strcmp(lower, "prev") == 0 || strcmp(lower, "previous") == 0)
    return UI_BUTTON_ICON_PREV;
  if (strcmp(lower, "back") == 0)
    return UI_BUTTON_ICON_BACK;
  if (strcmp(lower, "library") == 0)
    return UI_BUTTON_ICON_HOME;
  return UI_BUTTON_ICON_NONE;
}

int UiButtonSkin_IconBlockWidth(int buttonHeight) {
  int iconSide = buttonHeight - 8;
  if (iconSide < 10)
    iconSide = 10;
  if (iconSide > 26)
    iconSide = 26;
  return iconSide + 6;
}

void UiButtonSkin_Draw(u16 *screen, int stride, int logicalHeight, int x, int y,
                       int w, int h, UiButtonSkinState state, bool withIcon) {
  if (!screen || stride <= 0 || w <= 0 || h <= 0)
    return;
  if (!g_inited)
    UiButtonSkin_Init();

  ButtonCacheEntry *entry = get_or_build_cache(w, h, state, withIcon);
  if (!entry)
    return;

  // Alpha composite cached skin into software framebuffer with clipping.
  for (int py = 0; py < h; py++) {
    int sy = y + py;
    if (sy < 0 || sy >= logicalHeight)
      continue;
    for (int px = 0; px < w; px++) {
      int sx = x + px;
      if (sx < 0 || sx >= 240)
        continue;
      size_t sidx = (size_t)py * (size_t)w + (size_t)px;
      u8 a = entry->alpha[sidx];
      if (!a)
        continue;
      size_t didx = (size_t)sy * (size_t)stride + (size_t)sx;
      if (a == 255) {
        screen[didx] = entry->rgb565[sidx];
      } else {
        screen[didx] = Blend565(entry->rgb565[sidx], screen[didx], a);
      }
    }
  }
}

void UiButtonSkin_DrawIcon(u16 *screen, int stride, int logicalHeight, int x, int y,
                           int w, int h, UiButtonIconId icon, bool disabled) {
  if (!screen || stride <= 0 || w <= 0 || h <= 0 || icon < 0 ||
      icon >= UI_BUTTON_ICON_COUNT)
    return;
  if (!g_inited)
    UiButtonSkin_Init();
  try_load_icons_once();

  // Prefer external icon assets; fall back to procedural primitives if missing.
  const IconBitmap &bmp = g_icons[(int)icon];
  if (!bmp.loaded || bmp.width <= 0 || bmp.height <= 0) {
    draw_icon_fallback(screen, stride, logicalHeight, x, y, w, h, icon,
                       disabled);
    return;
  }

  int iconBlock = UiButtonSkin_IconBlockWidth(h);
  int boxSize = iconBlock - 8;
  if (boxSize < 8)
    boxSize = 8;
  int iconX = x + w - iconBlock + (iconBlock - boxSize) / 2;
  int iconY = y + (h - boxSize) / 2;

  for (int dy = 0; dy < boxSize; dy++) {
    int sy = iconY + dy;
    if (sy < 0 || sy >= logicalHeight)
      continue;
    int srcY = (dy * bmp.height) / boxSize;
    for (int dx = 0; dx < boxSize; dx++) {
      int sx = iconX + dx;
      if (sx < 0 || sx >= 240)
        continue;
      int srcX = (dx * bmp.width) / boxSize;
      size_t sidx = ((size_t)srcY * (size_t)bmp.width + (size_t)srcX) * 4u;
      u8 sr = bmp.rgba[sidx + 0];
      u8 sg = bmp.rgba[sidx + 1];
      u8 sb = bmp.rgba[sidx + 2];
      u8 sa = bmp.rgba[sidx + 3];
      if (sa == 0)
        continue;
      if (disabled)
        sa = (u8)((int)sa * 170 / 255);

      u16 fg = RGB565FromU8((float)sr, (float)sg, (float)sb);
      size_t didx = (size_t)sy * (size_t)stride + (size_t)sx;
      screen[didx] = Blend565(fg, screen[didx], sa);
    }
  }
}
