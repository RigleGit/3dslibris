#include "ui/font_manager.h"

#include <dirent.h>
#include <new>
#include <stdio.h>
#include <string>
#include <string.h>

#include "app/app.h"
#include "debug_log.h"
#include "shared/bugfix_utils.h"
#include "font_constants.h"
#include "path_utils.h"
#include "screen_constants.h"
#include "ui/text.h"
#include "ui/text_cache_utils.h"
#include "ui/text_limits.h"

namespace {

static bool FileReadable(const char *path) {
  if (!path || !*path)
    return false;
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  fclose(fp);
  return true;
}

static bool IsArchiveAbsolutePath(const std::string &path) {
  return path.find(":/") != std::string::npos ||
         (!path.empty() && path[0] == '/');
}

static std::string ResolveFontPath(const App *app, const std::string &filename) {
  if (filename.empty())
    return filename;
  if (IsArchiveAbsolutePath(filename))
    return filename;

  const std::string configured_dir =
      (app && !app->fontdir.empty()) ? app->fontdir : paths::GetFontDir();
  const std::string configured = configured_dir + "/" + filename;
  if (FileReadable(configured.c_str()))
    return configured;

  const std::string sdmc = paths::GetFontDir() + "/" + filename;
  if (FileReadable(sdmc.c_str()))
    return sdmc;

  const std::string romfs = std::string("romfs:/3ds/3dslibris/font/") + filename;
  if (FileReadable(romfs.c_str()))
    return romfs;

  return configured;
}

static FT_Error TextFaceRequester(FTC_FaceID face_id, FT_Library library,
                                  FT_Pointer request_data, FT_Face *aface) {
  (void)request_data;
  TextFace face = (TextFace)face_id;
  return FT_New_Face(library, face->file_path, face->face_index, aface);
}

}

FontManager::FontManager(Text *owner)
    : parent(owner), library(nullptr), error(0), ftc(false), charmap_index(0),
      pixelsize(12), fallback_count_(0) {
  for (int i = 0; i < kMaxFallbackFaces; i++)
    fallback_faces_[i] = nullptr;
  filenames[TEXT_STYLE_REGULAR] = FONTREGULARFILE;
  filenames[TEXT_STYLE_BOLD] = FONTBOLDFILE;
  filenames[TEXT_STYLE_ITALIC] = FONTITALICFILE;
  filenames[TEXT_STYLE_BOLDITALIC] = FONTBOLDITALICFILE;
  filenames[TEXT_STYLE_BROWSER] = FONTBROWSERFILE;
  filenames[TEXT_STYLE_MONO] = FONTMONOFILE;
  filenames[TEXT_STYLE_MONO_BOLD] = FONTMONOBOLDFILE;
  filenames[TEXT_STYLE_MONO_ITALIC] = FONTMONOITALICFILE;
  filenames[TEXT_STYLE_MONO_BOLDITALIC] = FONTMONOBOLDITALICFILE;

  if (parent) {
    ftc = false;
    pixelsize = parent->pixelsize;
    margin.left = parent->margin.left;
    margin.right = parent->margin.right;
    margin.top = parent->margin.top;
    margin.bottom = parent->margin.bottom;
  } else {
    margin.left = MARGINLEFT;
    margin.right = MARGINRIGHT;
    margin.top = MARGINTOP;
    margin.bottom = MARGINBOTTOM;
  }

  imagetype.face_id = (FTC_FaceID)&face_id;
  imagetype.flags = FT_LOAD_DEFAULT;
  imagetype.height = pixelsize;
  imagetype.width = 0;
}

FontManager::~FontManager() {
  UnloadFallbackFonts();
  ClearCache();
  for (std::map<FT_Face, Cache *>::iterator iter = textCache.begin();
       iter != textCache.end(); iter++) {
    delete iter->second;
  }
  textCache.clear();

  for (std::map<u8, FT_Face>::iterator iter = faces.begin(); iter != faces.end();
       iter++) {
    FT_Done_Face(iter->second);
  }
  if (library)
    FT_Done_FreeType(library);
}

FT_Face FontManager::GetFace(u8 style) {
  auto it = faces.find(style);
  return (it != faces.end()) ? it->second : NULL;
}

FT_Error FontManager::InitFreeTypeCache(void) {
  auto init_error = FT_Init_FreeType(&library);
  if (init_error)
    return init_error;
  init_error = FTC_Manager_New(library, 0, 0, 0, &TextFaceRequester, NULL,
                               &cache.manager);
  if (init_error)
    return init_error;
  init_error = FTC_ImageCache_New(cache.manager, &cache.image);
  if (init_error)
    return init_error;
  init_error = FTC_SBitCache_New(cache.manager, &cache.sbit);
  if (init_error)
    return init_error;
  init_error = FTC_CMapCache_New(cache.manager, &cache.cmap);
  if (init_error)
    return init_error;

  sprintf(face_id.file_path, "%s/%s", parent->app->fontdir.c_str(),
          filenames[TEXT_STYLE_REGULAR].c_str());
  face_id.face_index = 0;
  init_error =
      FTC_Manager_LookupFace(cache.manager, (FTC_FaceID)&face_id,
                             &faces[TEXT_STYLE_REGULAR]);
  if (init_error)
    return init_error;

  return 0;
}

FT_Error FontManager::CreateFace(int style) {
  const std::string path = ResolveFontPath(parent->app, filenames[style]);
  FT_Face loaded_face = nullptr;
  error = FT_New_Face(library, path.c_str(), 0, &loaded_face);
  if (error) {
    printf("[FAIL] Font(%d): %s\n", error, path.c_str());
    return error;
  }

  error = FT_Select_Charmap(loaded_face, FT_ENCODING_UNICODE);
  if (error) {
    printf("[FAIL] Charmap(%d): %s\n", error, path.c_str());
    FT_Done_Face(loaded_face);
    return error;
  }

  auto size = pixelsize;
  if (style == TEXT_STYLE_BROWSER)
    size = 12;

  error = FT_Set_Pixel_Sizes(loaded_face, 0, size);
  if (error) {
    printf("[FAIL] Pixel size(%d): %s\n", error, path.c_str());
    FT_Done_Face(loaded_face);
    return error;
  }

  Cache *new_cache = new (std::nothrow) Cache();
  if (!new_cache) {
    printf("[FAIL] Glyph cache alloc: %s\n", path.c_str());
    FT_Done_Face(loaded_face);
    return FT_Err_Out_Of_Memory;
  }

  FT_Face old_face = nullptr;
  std::map<u8, FT_Face>::iterator old_face_it = faces.find((u8)style);
  if (old_face_it != faces.end())
    old_face = old_face_it->second;

  faces[(u8)style] = loaded_face;
  textCache[loaded_face] = new_cache;
  advanceCache.erase(loaded_face);

  if (old_face && old_face != loaded_face) {
    ClearCache(old_face);
    std::map<FT_Face, Cache *>::iterator old_cache_it = textCache.find(old_face);
    if (old_cache_it != textCache.end()) {
      delete old_cache_it->second;
      textCache.erase(old_cache_it);
    }
    FT_Done_Face(old_face);
  }

  printf("[OK] Font: %s\n", path.c_str());

  return error;
}

int FontManager::InitCache(void) {
  error = FT_Init_FreeType(&library);
  if (error) {
    printf("[FAIL] FreeType init(%d)\n", error);
    return error;
  }

  const int styles[] = {TEXT_STYLE_BROWSER, TEXT_STYLE_REGULAR,
                        TEXT_STYLE_ITALIC, TEXT_STYLE_BOLD,
                        TEXT_STYLE_BOLDITALIC, TEXT_STYLE_MONO,
                        TEXT_STYLE_MONO_BOLD, TEXT_STYLE_MONO_ITALIC,
                        TEXT_STYLE_MONO_BOLDITALIC};
  for (int i = 0; i < (int)(sizeof(styles) / sizeof(styles[0])); i++) {
    FT_Error err = CreateFace(styles[i]);
    if (err && !error)
      error = err;
  }

  return error;
}

int FontManager::Init() {
  if (parent)
    pixelsize = parent->pixelsize;
  int err;
  if (ftc)
    err = InitFreeTypeCache();
  else
    err = InitCache();
  if (err == 0)
    AutoLoadCjkFallbackFonts();
  return err;
}

void FontManager::ReportFace(FT_Face face) {
  printf("%s\n", face->family_name);
  printf("%s\n", face->style_name);
  printf("faces %ld\n", face->num_faces);
  printf("glyphs %ld\n", face->num_glyphs);
  printf("fixed-sizes %d\n", face->num_fixed_sizes);
  for (int i = 0; i < face->num_fixed_sizes; i++) {
    printf(" w %d h %d\n", face->available_sizes[i].width,
           face->available_sizes[i].height);
  }
}

int FontManager::CacheGlyph(u32 ucs, FT_Face face) {
  Cache *face_cache = text_cache_utils::FindFaceCache(textCache, face);
  if (!face || !face_cache)
    return -1;
  uint32_t evicted_ucs = 0;
  if (face_cache->lru.Insert(ucs, &evicted_ucs)) {
    auto evicted =
        face_cache->cacheMap.find(evicted_ucs);
    if (evicted != face_cache->cacheMap.end()) {
      delete[] evicted->second->bitmap.buffer;
      delete evicted->second;
      face_cache->cacheMap.erase(evicted);
    }
#ifdef DSLIBRIS_DEBUG
    static unsigned int s_glyph_eviction_logs = 0;
    s_glyph_eviction_logs++;
    (void)s_glyph_eviction_logs;
#endif
  }

  FT_Load_Char(face, ucs, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL);
  FT_GlyphSlot src = face->glyph;
  FT_GlyphSlot dst = new FT_GlyphSlotRec;
  int x = src->bitmap.rows;
  int y = src->bitmap.width;
  dst->bitmap.buffer = new unsigned char[x * y];
  memcpy(dst->bitmap.buffer, src->bitmap.buffer, x * y);
  dst->bitmap.rows = src->bitmap.rows;
  dst->bitmap.width = src->bitmap.width;
  dst->bitmap_top = src->bitmap_top;
  dst->bitmap_left = src->bitmap_left;
  dst->advance = src->advance;
  face_cache->cacheMap.insert(std::make_pair(ucs, dst));
  return ucs;
}

FT_UInt FontManager::GetGlyphIndex(u32 ucs) {
  if (ftc)
    return FTC_CMapCache_Lookup(cache.cmap, (FTC_FaceID)&face_id, -1, ucs);
  FT_Face face = GetFace((u8)parent->GetStyle());
  if (!face)
    return 0;
  return FT_Get_Char_Index(face, ucs);
}

int FontManager::GetGlyphBitmap(u32 ucs, FTC_SBit *asbit, FTC_Node *anode) {
  if (!ftc)
    return 1;
  imagetype.face_id = (FTC_FaceID)&face_id;
  imagetype.height = pixelsize;
  imagetype.flags = FT_LOAD_RENDER;
  return FTC_SBitCache_Lookup(cache.sbit, &imagetype, GetGlyphIndex(ucs), asbit,
                              anode);
}

FT_GlyphSlot FontManager::GetGlyph(u32 ucs, int flags, FT_Face face) {
  if (ftc)
    halt(parent, "error: GetGlyph() called with ftc enabled");

  // Fast path: if glyph is already cached for the primary face, return it
  // immediately without any FreeType calls. This must happen before the ghost
  // check below, which would otherwise call FT_Load_Char on every glyph even
  // when the result is already in the cache.
  if (face && fallback_count_ > 0) {
    Cache *primary_cache = text_cache_utils::FindFaceCache(textCache, face);
    if (primary_cache) {
      auto fast_iter = primary_cache->cacheMap.find(ucs);
      if (fast_iter != primary_cache->cacheMap.end()) {
        if (parent->tr)
          parent->tr->SetHit(true);
        primary_cache->lru.Touch(ucs);
        return fast_iter->second;
      }
    }
  }

  // Try fallback if primary face lacks this glyph, or has a "ghost glyph"
  // (cmap entry exists but no ink — common in Unicode fonts for Arabic
  // Presentation Forms U+FE70-U+FEFF).
  // Ghost check is restricted to the Arabic Presentation Forms block where
  // ghost glyphs are actually observed, to avoid FT_Load_Char on every
  // Latin/Cyrillic/etc. codepoint.
  if (face && fallback_count_ > 0) {
    bool needs_fallback = !FT_Get_Char_Index(face, ucs);
    if (!needs_fallback && ucs >= 0xFE70u && ucs <= 0xFEFFu) {
      FT_Error load_err = FT_Load_Char(face, ucs, FT_LOAD_DEFAULT);
      if (!load_err && face->glyph->advance.x != 0 &&
          face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
          face->glyph->outline.n_contours == 0) {
        needs_fallback = true;
      }
    }
    if (needs_fallback) {
      FT_Face fb = FindFallbackFace(ucs);
      if (fb) {
        face = fb;
      }
    }
  }

  Cache *face_cache = text_cache_utils::FindFaceCache(textCache, face);
  if (!face || !face_cache) {
    if (parent->tr)
      parent->tr->SetHit(false);
    if (!face)
      return nullptr;
    FT_Load_Char(face, ucs, flags);
    return face->glyph;
  }
  auto iter = face_cache->cacheMap.find(ucs);
  if (iter != face_cache->cacheMap.end()) {
    if (parent->tr)
      parent->tr->SetHit(true);
    face_cache->lru.Touch(ucs);
    return iter->second;
  }

  if (parent->tr)
    parent->tr->SetHit(false);
  int i = CacheGlyph(ucs, face);
  if (i >= 0)
    return face_cache->cacheMap[ucs];

  FT_Load_Char(face, ucs, flags);
  return face->glyph;
}

void FontManager::ClearCache() {
  for (std::map<u8, FT_Face>::iterator iter = faces.begin(); iter != faces.end();
       iter++) {
    ClearCache(iter->second);
  }
  advanceCache.clear();
}

void FontManager::ClearCache(u8 style) { ClearCache(GetFace(style)); }

void FontManager::ClearRenderCache(FT_Face face) {
  Cache *face_cache = text_cache_utils::FindFaceCache(textCache, face);
  if (!face_cache)
    return;
  for (auto iter = face_cache->cacheMap.begin();
       iter != face_cache->cacheMap.end(); iter++) {
    delete[] iter->second->bitmap.buffer;
    delete iter->second;
  }
  face_cache->cacheMap.clear();
  face_cache->lru.Clear();
}

void FontManager::ClearCache(FT_Face face) {
  ClearRenderCache(face);
  advanceCache.erase(face);
}

u8 FontManager::GetAdvance(u32 ucs, FT_Face face) {
  if (ftc) {
    auto gindex = GetGlyphIndex(ucs);

    FTC_SBit psbit;
    error = FTC_SBitCache_Lookup(cache.sbit, &imagetype, gindex, &psbit, NULL);
    if (!error)
      return psbit->xadvance;

    FT_Glyph glyph;
    error = FTC_ImageCache_Lookup(cache.image, &imagetype, gindex, &glyph, NULL);
    if (!error)
      return (glyph->advance).x;
  } else {
    bool needs_fallback = false;
#ifdef ADVANCE_NO_CACHE
    auto gindex = FT_Get_Char_Index(face, ucs);
    if (!gindex) {
      needs_fallback = true;
    } else {
      error = FT_Load_Glyph(face, gindex, FT_LOAD_DEFAULT);
      if (!error) {
        bool ghost = face->glyph->advance.x != 0 &&
                     face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
                     face->glyph->outline.n_contours == 0;
        if (!ghost)
          return face->glyph->advance.x >> 6;
        needs_fallback = true;
      }
    }
#else
    const u32 cache_key = ((u32)pixelsize << 24) | (ucs & 0x00FFFFFFu);
    auto &faceAdvanceCache = advanceCache[face];
    auto iter = faceAdvanceCache.find(cache_key);
    if (iter != faceAdvanceCache.end())
      return iter->second;

    if (!FT_Get_Char_Index(face, ucs)) {
      needs_fallback = true;
    } else {
      error = FT_Load_Char(face, ucs, FT_LOAD_DEFAULT);
      if (!error) {
        bool ghost = face->glyph->advance.x != 0 &&
                     face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
                     face->glyph->outline.n_contours == 0;
        if (!ghost) {
          u8 advance = face->glyph->advance.x >> 6;
          faceAdvanceCache.insert(std::make_pair(cache_key, advance));
          return advance;
        }
        needs_fallback = true;
      }
    }
#endif

    if (needs_fallback && fallback_count_ > 0) {
      FT_Face fb = FindFallbackFace(ucs);
      if (fb) {
#ifdef ADVANCE_NO_CACHE
        error = FT_Load_Char(fb, ucs, FT_LOAD_DEFAULT);
        if (!error)
          return fb->glyph->advance.x >> 6;
#else
        auto &fbAdvanceCache = advanceCache[fb];
        auto fb_iter = fbAdvanceCache.find(cache_key);
        if (fb_iter != fbAdvanceCache.end()) {
          faceAdvanceCache.insert(std::make_pair(cache_key, fb_iter->second));
          return fb_iter->second;
        }

        error = FT_Load_Char(fb, ucs, FT_LOAD_DEFAULT);
        if (!error) {
          u8 advance = fb->glyph->advance.x >> 6;
          fbAdvanceCache.insert(std::make_pair(cache_key, advance));
          faceAdvanceCache.insert(std::make_pair(cache_key, advance));
          return advance;
        }
#endif
      }
    }
  }

  return 0;
}

u8 FontManager::GetStringWidth(const char *txt, FT_Face face) {
  if (!txt)
    return 0;

  const size_t len = strlen(txt);
  int width = 0;
  size_t offset = 0;
  while (offset < len) {
    u32 ucs = 0;
    u8 bytes = parent->GetCharCode(txt + offset, len - offset, &ucs);
    if (bytes == 0) {
      offset++;
      continue;
    }
    width += GetAdvance(ucs, face);
    offset += bytes;
  }

  if (width < 0)
    return 0;
  if (width > 255)
    return 255;
  return (u8)width;
}

u8 FontManager::GetHeight() {
  FT_Face face = GetFace((u8)parent->GetStyle());
  if (!face || !face->size)
    return 0;
  return (face->size->metrics.height >> 6);
}

std::string FontManager::GetFontName(u8 style) {
  FT_Face face = GetFace(style);
  if (!face)
    return "";
  return std::string(face->family_name) + " " +
         std::string(face->style_name);
}

bool FontManager::GetFontName(std::string &s) {
  FT_Face face = GetFace((u8)parent->GetStyle());
  if (!face)
    return false;
  const char *name = FT_Get_Postscript_Name(face);
  if (!name)
    return false;
  s = name;
  return true;
}

void FontManager::SetFontFile(const char *path, u8 style) {
  if (!path || !*path)
    return;
  if (!strcmp(filenames[style].c_str(), path))
    return;
  const std::string previous = filenames[style];
  filenames[style] = std::string(path);
  FT_Error err = CreateFace(style);
  if (err) {
    // Keep runtime/prefs consistency: do not leave a filename selected when
    // the face could not be created.
    filenames[style] = previous;
  }
}

std::string FontManager::GetFontFile(u8 style) { return filenames[style]; }

void FontManager::SetPixelSize(u8 size) {
  size = (u8)ClampTextPixelSize((int)size);
  if (size == pixelsize)
    return;
  pixelsize = size;
  if (parent)
    parent->pixelsize = size;
  if (ftc) {
    imagetype.height = pixelsize;
    imagetype.width = pixelsize;
  } else {
    for (auto &it : faces) {
      if (it.first == TEXT_STYLE_BROWSER)
        continue;
      FT_Set_Pixel_Sizes(it.second, 0, pixelsize);
      ClearRenderCache(it.second);
    }
  }
}

int FontManager::GetPixelSize() const { return pixelsize; }

bool FontManager::LoadFallbackFont(const char *path) {
  if (!path || !*path)
    return false;
  if (fallback_count_ >= kMaxFallbackFaces) {
    printf("[WARN] Fallback font limit reached (%d)\n", kMaxFallbackFaces);
    DBG_LOGF(parent ? parent->app : nullptr,
             "[WARN] Fallback font limit reached (%d)", kMaxFallbackFaces);
    return false;
  }

  int slot = -1;
  for (int i = 0; i < kMaxFallbackFaces; i++) {
    if (!fallback_faces_[i]) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return false;
  return SetFallbackFile(slot, path);
}

bool FontManager::SetFallbackFile(int index, const char *path) {
  if (index < 0 || index >= kMaxFallbackFaces || !path || !*path)
    return false;

  const std::string resolved = ResolveFontPath(parent ? parent->app : nullptr, path);
  if (!FileReadable(resolved.c_str())) {
    printf("[WARN] Fallback font not found: %s\n", resolved.c_str());
    return false;
  }

  FT_Face face = nullptr;
  FT_Error err = FT_New_Face(library, resolved.c_str(), 0, &face);
  if (err || !face) {
    printf("[FAIL] Fallback font(%d): %s\n", err, resolved.c_str());
    return false;
  }

  err = FT_Select_Charmap(face, FT_ENCODING_UNICODE);
  if (err) {
    printf("[FAIL] Fallback charmap(%d): %s\n", err, resolved.c_str());
    FT_Done_Face(face);
    return false;
  }

  err = FT_Set_Pixel_Sizes(face, 0, pixelsize);
  if (err) {
    printf("[FAIL] Fallback pixel size(%d): %s\n", err, resolved.c_str());
    FT_Done_Face(face);
    return false;
  }

  Cache *new_cache = new (std::nothrow) Cache();
  if (!new_cache) {
    FT_Done_Face(face);
    return false;
  }

  FT_Face old_face = fallback_faces_[index];
  if (old_face) {
    ClearCache(old_face);
    std::map<FT_Face, Cache *>::iterator it = textCache.find(old_face);
    if (it != textCache.end()) {
      delete it->second;
      textCache.erase(it);
    }
    advanceCache.erase(old_face);
    FT_Done_Face(old_face);
  }

  fallback_faces_[index] = face;
  fallback_filenames_[index] = std::string(path);
  textCache[face] = new_cache;
  if (index + 1 > fallback_count_)
    fallback_count_ = index + 1;

  printf("[OK] Fallback font[%d]: %s\n", index, resolved.c_str());
  DBG_LOGF(parent ? parent->app : nullptr,
           "[OK] Fallback font[%d]: %s", index, resolved.c_str());
  return true;
}

void FontManager::UnloadFallbackFonts() {
  for (int i = 0; i < kMaxFallbackFaces; i++) {
    FT_Face face = fallback_faces_[i];
    if (!face)
      continue;
    ClearCache(face);
    std::map<FT_Face, Cache *>::iterator it = textCache.find(face);
    if (it != textCache.end()) {
      delete it->second;
      textCache.erase(it);
    }
    advanceCache.erase(face);
    FT_Done_Face(face);
    fallback_faces_[i] = nullptr;
    fallback_filenames_[i].clear();
  }
  fallback_count_ = 0;
}

int FontManager::GetFallbackCount() const { return fallback_count_; }

std::string FontManager::GetFallbackFile(int index) const {
  if (index < 0 || index >= kMaxFallbackFaces)
    return std::string();
  return fallback_filenames_[index];
}

FT_Face FontManager::FindFallbackFace(u32 ucs) {
  for (int i = 0; i < kMaxFallbackFaces; i++) {
    if (fallback_faces_[i] && FT_Get_Char_Index(fallback_faces_[i], ucs))
      return fallback_faces_[i];
  }
  return nullptr;
}

static bool FilenameMatchesCjkPattern(const char *filename) {
  if (!filename)
    return false;
  for (int i = 0; i < paths::kCjkFontPatternCount; i++) {
    if (strstr(filename, paths::kCjkFontPatterns[i]))
      return true;
  }
  return false;
}

static void AutoLoadFallbackFontsFromDir(FontManager *fm, const std::string &font_dir,
                                         int *loaded) {
  if (!fm || !loaded || font_dir.empty())
    return;

  DIR *dp = opendir(font_dir.c_str());
  if (!dp)
    return;

  struct dirent *ent;
  while ((ent = readdir(dp)) && *loaded < FontManager::kMaxFallbackFaces) {
    if (ent->d_type == DT_DIR)
      continue;
    const char *name = ent->d_name;
    const char *ext = strrchr(name, '.');
    if (!ext || (strcmp(ext, ".ttf") && strcmp(ext, ".otf") && strcmp(ext, ".ttc")))
      continue;
    if (!FilenameMatchesCjkPattern(name))
      continue;

    bool already_loaded = false;
    for (int i = 0; i < FontManager::kMaxFallbackFaces; i++) {
      const std::string existing = fm->GetFallbackFile(i);
      if (existing.empty())
        continue;
      const char *existing_base = strrchr(existing.c_str(), '/');
      existing_base = existing_base ? existing_base + 1 : existing.c_str();
      if (!strcmp(existing_base, name)) {
        already_loaded = true;
        break;
      }
    }
    if (already_loaded)
      continue;

    std::string full_path = font_dir + "/" + name;
    if (fm->LoadFallbackFont(full_path.c_str()))
      (*loaded)++;
  }

  closedir(dp);
}

void FontManager::AutoLoadCjkFallbackFonts() {
  const std::string font_dir =
      (parent && parent->app && !parent->app->fontdir.empty())
          ? parent->app->fontdir
          : std::string();
  std::vector<std::string> search_dirs;
  GetFallbackFontSearchDirs(font_dir, paths::GetFontDir().c_str(), &search_dirs);
  int loaded = 0;
  for (size_t i = 0; i < search_dirs.size() && loaded < kMaxFallbackFaces; i++)
    AutoLoadFallbackFontsFromDir(this, search_dirs[i], &loaded);

  if (loaded > 0) {
    printf("[OK] Auto-loaded %d CJK fallback font(s)\n", loaded);
    DBG_LOGF(parent ? parent->app : nullptr,
             "[OK] Auto-loaded %d CJK fallback font(s)", loaded);
  }
}
