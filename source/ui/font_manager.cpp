#include "ui/font_manager.h"

#include <new>
#include <stdio.h>
#include <string>
#include <string.h>

#include "app/app.h"
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
      (app && !app->fontdir.empty()) ? app->fontdir : std::string(paths::kFontDir);
  const std::string configured = configured_dir + "/" + filename;
  if (FileReadable(configured.c_str()))
    return configured;

  const std::string sdmc = std::string(paths::kFontDir) + "/" + filename;
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
      pixelsize(12) {
  filenames[TEXT_STYLE_REGULAR] = FONTREGULARFILE;
  filenames[TEXT_STYLE_BOLD] = FONTBOLDFILE;
  filenames[TEXT_STYLE_ITALIC] = FONTITALICFILE;
  filenames[TEXT_STYLE_BOLDITALIC] = FONTBOLDITALICFILE;
  filenames[TEXT_STYLE_BROWSER] = FONTBROWSERFILE;

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

FT_Face FontManager::GetFace(u8 style) { return faces[style]; }

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
                        TEXT_STYLE_BOLDITALIC};
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
  if (ftc)
    return InitFreeTypeCache();
  else
    return InitCache();
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
  else
    return FT_Get_Char_Index(GetFace((u8)parent->GetStyle()), ucs);
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

void FontManager::ClearCache(FT_Face face) {
  Cache *face_cache = text_cache_utils::FindFaceCache(textCache, face);
  if (!face_cache) {
    advanceCache.erase(face);
    return;
  }

  for (auto iter = face_cache->cacheMap.begin();
       iter != face_cache->cacheMap.end(); iter++) {
    delete[] iter->second->bitmap.buffer;
    delete iter->second;
  }
  face_cache->cacheMap.clear();
  face_cache->lru.Clear();
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
#ifdef ADVANCE_NO_CACHE
    auto gindex = FT_Get_Char_Index(face, ucs);
    error = FT_Load_Glyph(face, gindex, FT_LOAD_DEFAULT);
    if (!error)
      return face->glyph->advance.x >> 6;
#else
    auto &faceAdvanceCache = advanceCache[face];
    auto iter = faceAdvanceCache.find(ucs);
    if (iter != faceAdvanceCache.end())
      return iter->second;

    error = FT_Load_Char(face, ucs, FT_LOAD_DEFAULT);
    if (!error) {
      u8 advance = face->glyph->advance.x >> 6;
      faceAdvanceCache.insert(std::make_pair(ucs, advance));
      return advance;
    }
#endif
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
  return (GetFace((u8)parent->GetStyle())->size->metrics.height >> 6);
}

std::string FontManager::GetFontName(u8 style) {
  return std::string(faces[style]->family_name) + " " +
         std::string(faces[style]->style_name);
}

bool FontManager::GetFontName(std::string &s) {
  const char *name = FT_Get_Postscript_Name(GetFace((u8)parent->GetStyle()));
  if (!name)
    return false;
  s = name;
  return true;
}

void FontManager::SetFontFile(const char *path, u8 style) {
  if (!strcmp(filenames[style].c_str(), path))
    return;
  filenames[style] = std::string(path);
  CreateFace(style);
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
      ClearCache(it.second);
    }
  }
}

int FontManager::GetPixelSize() const { return pixelsize; }
