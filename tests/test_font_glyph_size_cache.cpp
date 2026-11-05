#include "ui/text.h"
#include "ui/font_constants.h"
#include FT_SIZES_H
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

// Observe the real dependency boundary without adding production test hooks.
// The runner renames these two API references in FontManager only; the oracle
// below calls the unmodified FreeType APIs.
#undef FT_Set_Pixel_Sizes
#undef FT_New_Size
extern "C" FT_Error FT_Set_Pixel_Sizes(FT_Face, FT_UInt, FT_UInt);
extern "C" FT_Error FT_New_Size(FT_Face, FT_Size *);
static size_t pixel_size_setups=0, size_allocations=0;
static bool reject_size_allocation=false;
extern "C" FT_Error ObserveSetPixelSizes(FT_Face face, FT_UInt width,
                                         FT_UInt height) {
  ++pixel_size_setups;
  return FT_Set_Pixel_Sizes(face,width,height);
}
extern "C" FT_Error ObserveNewSize(FT_Face face, FT_Size *size) {
  if (reject_size_allocation) {
    *size=nullptr;
    return FT_Err_Out_Of_Memory;
  }
  FT_Error error=FT_New_Size(face,size);
  if (!error) ++size_allocations;
  return error;
}

// Real FontManager and FreeType; Text is configuration only for this owner test.
Text::Text() : reporter_(nullptr), fm(nullptr), tr(nullptr) {
  pixelsize=12;
  margin.left=margin.right=margin.top=margin.bottom=0;
}
Text::~Text() {}
int Text::GetStyle() { std::abort(); }
u8 Text::GetCharCode(const char *, size_t, u32 *) { std::abort(); }
void TextRenderer::SetHit(bool) { std::abort(); }
int halt(Text *, const char *, int) { std::abort(); }

// Count requests at FreeType's stream boundary while returning the original
// bytes. macOS can memory-map fonts, so give memory streams an equivalent read
// callback as well. Restore their original state before face replacement/close.
static size_t reads=0, bytes=0;
static std::map<FT_Stream, FT_Stream_IoFunc> streams;
static std::map<FT_Stream, const unsigned char *> stream_bases;
static unsigned long CountRead(FT_Stream stream, unsigned long offset,
                                unsigned char *buffer, unsigned long count) {
  if (count) { ++reads; bytes+=count; }
  FT_Stream_IoFunc original=streams.at(stream);
  if (original) return original(stream, offset, buffer, count);
  if (offset>stream->size) return count?0:1;
  count=std::min(count,stream->size-offset);
  if (count) std::memcpy(buffer,stream_bases.at(stream)+offset,count);
  return count;
}
static void Observe(FT_Face face) {
  if (streams.count(face->stream)) return;
  streams[face->stream]=face->stream->read;
  stream_bases[face->stream]=face->stream->base;
  assert(streams[face->stream] || face->stream->base);
  face->stream->read=CountRead;
}
static void Unobserve(FT_Face face) {
  auto it=streams.find(face->stream);
  if (it==streams.end()) return;
  face->stream->read=it->second;
  face->stream->base=const_cast<unsigned char *>(stream_bases.at(face->stream));
  streams.erase(it);
  stream_bases.erase(face->stream);
}
static void ExpectGlyph(FT_GlyphSlot actual, FT_GlyphSlot expected) {
  assert(actual && expected);
  assert(actual->bitmap.rows==expected->bitmap.rows);
  assert(actual->bitmap.width==expected->bitmap.width);
  assert(actual->bitmap_top==expected->bitmap_top);
  assert(actual->bitmap_left==expected->bitmap_left);
  assert(actual->advance.x==expected->advance.x);
  assert(actual->advance.y==expected->advance.y);
  for (unsigned y=0; y<expected->bitmap.rows; ++y)
    assert(!std::memcmp(actual->bitmap.buffer+y*actual->bitmap.width,
                       expected->bitmap.buffer+y*expected->bitmap.pitch,
                       expected->bitmap.width));
}
// Measuring blank glyphs through a fallback underestimates large-font lines.
// Compare both public paths with an independent primary FreeType face, so a
// matching but equally wrong measurement/render pair cannot hide the error.
static void ExpectPrimaryAdvances(FontManager &manager, FT_Library library,
                                  const char *fontdir, const char *fallback) {
  FT_Face oracle;
  const std::string path=std::string(fontdir)+"/"+
                         manager.GetFontFile(TEXT_STYLE_REGULAR);
  assert(!FT_New_Face(library,path.c_str(),0,&oracle));
  unsigned mismatches=0;
  for (bool with_fallback : {false,true}) {
    manager.UnloadFallbackFonts();
    manager.SetPixelSize(12);
    if (with_fallback) assert(manager.SetFallbackFile(0,fallback));
    manager.ClearCache();
    for (u8 px=8; px<=20; ++px) {
      manager.SetPixelSize(px);
      FT_Face face=manager.GetFace(TEXT_STYLE_REGULAR);
      assert(!FT_Set_Pixel_Sizes(oracle,0,px));
      for (u32 cp : {u32(' '),u32(0x00a0),u32(0x2002),u32('A'),
                     u32(0x2019)}) {
        assert(FT_Get_Char_Index(oracle,cp));
        assert(!FT_Load_Char(oracle,cp,FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL));
        const int expected=oracle->glyph->advance.x>>6;
        // Repeat to cover advance-cache hits as well as misses.
        for (int pass=0; pass<2; ++pass) {
          const int measured=manager.GetAdvance(cp,face);
          FT_GlyphSlot glyph=manager.GetGlyph(cp,FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL,face);
          if (measured!=expected || !glyph || (glyph->advance.x>>6)!=expected) {
            std::fprintf(stderr,"advance U+%04x px=%u fallback=%d: measured=%d expected=%d\n",
                         cp,px,with_fallback,measured,expected);
            ++mismatches;
          }
        }
      }
    }
  }
  manager.SetPixelSize(14);
  FT_Face face=manager.GetFace(TEXT_STYLE_REGULAR);
  int width=0;
  for (const char *s="you last spring, I will not watch one of"; *s; ++s)
    width+=manager.GetAdvance(static_cast<unsigned char>(*s),face);
  if (width<=214) {
    std::fprintf(stderr,"issue #172: line measured %d px, must exceed 214 px\n",width);
    ++mismatches;
  }
  FT_Done_Face(oracle);
  manager.UnloadFallbackFonts();
  assert(mismatches==0);
}
int main() {
  const char *fontdir=std::getenv("TEST_ADVANCE_FONT_DIR");
  const char *fallback=std::getenv("TEST_ADVANCE_FALLBACK");
  assert(fontdir && fallback);
  {
    Text staged;
    FontManager configured(&staged);
    configured.SetFontDir(fontdir);
    configured.SetFontFile("LiberationSerif-Bold.ttf", TEXT_STYLE_REGULAR);
    configured.SetPixelSize(16);
    assert(configured.SetFallbackFile(0, fallback));
    configured.AutoLoadCjkFallbackFonts();
    assert(!configured.GetFace(TEXT_STYLE_REGULAR));
    assert(!configured.Init());
    const FT_Face selected = configured.GetFace(TEXT_STYLE_REGULAR);
    assert(selected && (selected->style_flags & FT_STYLE_FLAG_BOLD));
    assert(selected->size->metrics.y_ppem == 16);
    assert(configured.GetFallbackFile(0) == fallback);
    assert(configured.GetFallbackCount() > 0);
    // Rejecting an invalid setting before mono's first use must leave the
    // bundled mono face available for its subsequent lazy load.
    const u32 revision=configured.GetRevision();
    configured.SetFontFile("missing-font.ttf", TEXT_STYLE_MONO);
    assert(configured.GetRevision()==revision);
    assert(configured.GetFontFile(TEXT_STYLE_MONO) == FONTMONOFILE);
    const FT_Face mono = configured.GetFace(TEXT_STYLE_MONO);
    assert(mono && FT_IS_FIXED_WIDTH(mono));
  }
  Text text;
  FontManager manager(&text);
  manager.SetFontDir(fontdir);
  assert(!manager.Init());
  manager.UnloadFallbackFonts();
  FT_Library library;
  assert(!FT_Init_FreeType(&library));
  const int flags=FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL;
  ExpectPrimaryAdvances(manager,library,fontdir,fallback);
  {
    FT_Face oracle;
    const std::string path=std::string(fontdir)+"/"+
                           manager.GetFontFile(TEXT_STYLE_REGULAR);
    assert(!FT_New_Face(library,path.c_str(),0,&oracle));
    const u32 revision=manager.GetRevision();
    // Failure to allocate the second size must keep native rendering working.
    reject_size_allocation=true;
    manager.SetPixelSize(16);
    FT_Face face=manager.GetFace(TEXT_STYLE_REGULAR);
    reject_size_allocation=false;
    assert(!FT_Set_Pixel_Sizes(oracle,0,16));
    assert(!FT_Load_Char(oracle,'A',flags));
    ExpectGlyph(manager.GetGlyph('A',flags,face),oracle->glyph);
    const size_t allocations_before=size_allocations;
    // A 16px body uses 11px scripts (70%, truncated). Prime both first, then
    // repeated draws must need no size setup or additional size allocation.
    for (int pass=0; pass<9; ++pass) {
      pixel_size_setups=0;
      for (u8 px : {16,11}) {
        manager.SetPixelSize(px);
        face=manager.GetFace(TEXT_STYLE_REGULAR);
        assert(!FT_Set_Pixel_Sizes(oracle,0,px));
        for (u32 cp : {u32('A'),u32('g'),u32(0x00e9)}) {
          assert(!FT_Load_Char(oracle,cp,flags));
          ExpectGlyph(manager.GetGlyph(cp,flags,face),oracle->glyph);
          assert(manager.GetAdvance(cp,face)==(oracle->glyph->advance.x>>6));
        }
      }
      if (pass) assert(pixel_size_setups==0);
      assert(size_allocations<=allocations_before+1);
      assert(manager.GetRevision()==revision);
    }
    FT_Done_Face(oracle);
  }
  // Every supported size, face style and revisited size must retain exactly
  // the native bitmap. Browser stays at 12px while other faces change size.
  for (u8 style : {TEXT_STYLE_REGULAR, TEXT_STYLE_BOLD, TEXT_STYLE_BROWSER}) {
    FT_Face face=manager.GetFace(style), oracle;
    std::string path=std::string(fontdir)+"/"+manager.GetFontFile(style);
    assert(!FT_New_Face(library,path.c_str(),0,&oracle));
    Observe(face);
    const size_t allocations_before=size_allocations;
    for (int pass=0; pass<2; ++pass) {
      for (u8 px=8; px<=20; ++px) {
        manager.SetPixelSize(px);
        face=manager.GetFace(style);
        assert(!FT_Set_Pixel_Sizes(oracle,0,style==TEXT_STYLE_BROWSER?12:px));
        for (u32 cp : {u32('A'),u32('g'),u32(0x00e9)}) {
          assert(!FT_Load_Char(oracle,cp,flags));
          reads=bytes=0;
          ExpectGlyph(manager.GetGlyph(cp,flags,face),oracle->glyph);
          if (pass) assert(reads==0 && "revisited size reloads cached glyph data");
        }
      }
    }
    assert(size_allocations<=allocations_before+1);
    FT_Done_Face(oracle);
  }
  // Explicit invalidation and replacement must discard all sizes of a face.
  FT_Face regular=manager.GetFace(TEXT_STYLE_REGULAR);
  manager.ClearCache(TEXT_STYLE_REGULAR);
  reads=0;
  manager.GetGlyph('A',flags,regular);
  assert(reads>0);
  Unobserve(regular);
  const u32 revision=manager.GetRevision();
  manager.SetFontFile("LiberationSerif-Bold.ttf",TEXT_STYLE_REGULAR);
  assert(manager.GetRevision()!=revision);
  regular=manager.GetFace(TEXT_STYLE_REGULAR);
  // Old stream addresses may be reused after replacement.
  Observe(regular);
  reads=0;
  manager.GetGlyph('A',flags,regular);
  assert(reads>0);
  // The existing 512-entry LRU budget is shared by all sizes, not multiplied.
  manager.ClearCache(TEXT_STYLE_REGULAR);
  manager.SetPixelSize(10);
  for (u32 cp=33; cp<545; ++cp) manager.GetGlyph(cp,flags,regular);
  manager.SetPixelSize(12);
  manager.GetGlyph(600,flags,regular);
  manager.SetPixelSize(10);
  regular=manager.GetFace(TEXT_STYLE_REGULAR);
  reads=0;
  manager.GetGlyph(544,flags,regular);
  assert(reads==0);
  manager.GetGlyph(33,flags,regular);
  assert(reads>0);
  // A real fallback remains native at its own active size across primary
  // size changes, and replacing/unloading it retires its cached bitmaps.
  const u32 before_fallback=manager.GetRevision();
  assert(manager.SetFallbackFile(0,fallback));
  assert(manager.GetRevision()!=before_fallback);
  FT_Face oracle;
  assert(!FT_New_Face(library,fallback,0,&oracle));
  assert(!FT_Set_Pixel_Sizes(oracle,0,10));
  assert(!FT_Load_Char(oracle,0x0531,flags));
  for (u8 px : {10,18,10}) {
    manager.SetPixelSize(px);
    ExpectGlyph(manager.GetGlyph(0x0531,flags,regular),oracle->glyph);
  }
  const u32 before_replacement=manager.GetRevision();
  assert(manager.SetFallbackFile(0,fallback));
  assert(manager.GetRevision()!=before_replacement);
  ExpectGlyph(manager.GetGlyph(0x0531,flags,regular),oracle->glyph);
  const u32 before_unload=manager.GetRevision();
  manager.UnloadFallbackFonts();
  assert(manager.GetRevision()!=before_unload);
  FT_Done_Face(oracle);
  for (u8 style : {TEXT_STYLE_REGULAR, TEXT_STYLE_BOLD, TEXT_STYLE_BROWSER})
    Unobserve(manager.GetFace(style));
  FT_Done_FreeType(library);
  puts("PASS: glyph pixels/advances, bounded size reuse, font revisions, shared LRU budget, face/fallback retirement");
}
