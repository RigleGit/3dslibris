#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

#include <3ds.h>

#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_CACHE_H

#include "shared/path_constants.h"
#include "ui/glyph_cache_lru.h"

class Text;

typedef struct TextFaceRec_ {
  char file_path[128];
  int face_index;
} TextFaceRec, *TextFace;

typedef struct TextCache_ {
  FTC_Manager manager;
  FTC_CMapCache cmap;
  FTC_ImageCache image;
  FTC_SBitCache sbit;
} TextCache;

class Cache {
public:
  std::unordered_map<u32, FT_GlyphSlot> cacheMap;
  glyph_cache_lru::GlyphCacheLru lru;
  std::array<FT_GlyphSlot, 256> latin1_slots;
  std::array<u32, 256> latin1_keys;
  Cache() : lru(512), latin1_slots{}, latin1_keys{} {}

  FT_GlyphSlot FindGlyph(u32 ucs, u32 key) {
    if (ucs < latin1_slots.size() && latin1_slots[ucs] &&
        latin1_keys[ucs] == key)
      return latin1_slots[ucs];
    const auto it = cacheMap.find(key);
    if (it == cacheMap.end())
      return nullptr;
    RememberLatin1(ucs, key, it->second);
    return it->second;
  }
  void RememberLatin1(u32 ucs, u32 key, FT_GlyphSlot slot) {
    if (ucs < latin1_slots.size()) {
      latin1_keys[ucs] = key;
      latin1_slots[ucs] = slot;
    }
  }
  void ForgetLatin1(u32 key) {
    const u32 ucs = key & 0x00FFFFFFu;
    if (ucs < latin1_slots.size() && latin1_keys[ucs] == key)
      latin1_slots[ucs] = nullptr;
  }
};

class FontManager {
public:
  static const int kMaxFallbackFaces = 8;

  FontManager(Text *parent);
  ~FontManager();

  FT_Face GetFace(u8 style);
  u8 GetAdvance(u32 ucs, FT_Face face);
  FT_UInt GetGlyphIndex(u32 ucs);
  u8 GetStringWidth(const char *txt, FT_Face face);
  u8 GetHeight();
  std::string GetFontName(u8 style);
  bool GetFontName(std::string &s);
  void SetFontFile(const char *path, u8 style);
  std::string GetFontFile(u8 style);
  void SetPixelSize(u8 size);
  int GetPixelSize() const;

  FT_Error CreateFace(int style);
  int CacheGlyph(u32 ucs, FT_Face face);
  FT_GlyphSlot GetGlyph(u32 ucs, int flags, FT_Face face);
  void ClearCache();
  void ClearCache(u8 style);
  void ClearCache(FT_Face face);
  void ClearRenderCache(FT_Face face);
  int GetGlyphBitmap(u32 ucs, FTC_SBit *sbit, FTC_Node *anode = nullptr);

  // Font fallback for CJK/Hebrew/Arabic scripts.
  bool LoadFallbackFont(const char *path);
  bool SetFallbackFile(int index, const char *path);
  void UnloadFallbackFonts();
  int GetFallbackCount() const;
  std::string GetFallbackFile(int index) const;
  void AutoLoadCjkFallbackFonts();

  int Init();
  void ReportFace(FT_Face face);
  void SetFontDir(const std::string &dir);
  const std::string &GetFontDir() const;

private:
  Text *parent;
  FT_Library library;
  FT_Error error;
  bool ftc;
  std::array<FT_Face, 9> faces;
  std::array<bool, 9> face_load_attempted_;
  std::map<u8, std::string> filenames;
  std::map<FT_Face, Cache *> textCache;
  FT_Face cached_face_ = nullptr;
  Cache *cached_face_data_ = nullptr;
  std::map<FT_Face, std::unordered_map<u32, u8>> advanceCache;
#ifdef DSLIBRIS_DEBUG
  struct AdvancePerf {
    uint64_t calls = 0, hits = 0, misses = 0, loads = 0;
    uint64_t fallback_searches = 0, fallback_hits = 0, unresolved = 0;
  } advance_perf_;
  void LogAdvancePerf();
#endif
  TextCache cache;
  TextFaceRec face_id;
  FTC_SBitRec sbit;
  FTC_ImageTypeRec imagetype;
  FT_Int charmap_index;
  int pixelsize;
  struct {
    int left, right, top, bottom;
  } margin;

  // Fallback font faces for scripts not covered by primary fonts.
  FT_Face fallback_faces_[kMaxFallbackFaces];
  std::string fallback_filenames_[kMaxFallbackFaces];
  int fallback_count_;
  std::string fontdir_;

  FT_Face FindFallbackFace(u32 ucs);
  void EnsureFacePixelSize(FT_Face face);
  Cache *GetFaceCache(FT_Face face);
  FT_Error CreateConfiguredFace(int style);

  FT_Error InitFreeTypeCache();
  int InitCache();
};
