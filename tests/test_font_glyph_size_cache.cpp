#include "ui/text.h"
#include "ui/font_constants.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

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
int main() {
  const char *fontdir=std::getenv("TEST_ADVANCE_FONT_DIR");
  const char *fallback=std::getenv("TEST_ADVANCE_FALLBACK");
  assert(fontdir && fallback);
  Text text;
  FontManager manager(&text);
  manager.SetFontDir(fontdir);
  assert(!manager.Init());
  manager.UnloadFallbackFonts();
  FT_Library library;
  assert(!FT_Init_FreeType(&library));
  const int flags=FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL;
  // Every supported size, face style and revisited size must retain exactly
  // the native bitmap. Browser stays at 12px while other faces change size.
  for (u8 style : {TEXT_STYLE_REGULAR, TEXT_STYLE_BOLD, TEXT_STYLE_BROWSER}) {
    FT_Face face=manager.GetFace(style), oracle;
    std::string path=std::string(fontdir)+"/"+manager.GetFontFile(style);
    assert(!FT_New_Face(library,path.c_str(),0,&oracle));
    Observe(face);
    for (int pass=0; pass<2; ++pass) {
      for (u8 px=8; px<=20; ++px) {
        manager.SetPixelSize(px);
        assert(!FT_Set_Pixel_Sizes(oracle,0,style==TEXT_STYLE_BROWSER?12:px));
        for (u32 cp : {u32('A'),u32('g'),u32(0x00e9)}) {
          assert(!FT_Load_Char(oracle,cp,flags));
          reads=bytes=0;
          ExpectGlyph(manager.GetGlyph(cp,flags,face),oracle->glyph);
          if (pass) assert(reads==0 && "revisited size reloads cached glyph data");
        }
      }
    }
    FT_Done_Face(oracle);
  }
  // Explicit invalidation and replacement must discard all sizes of a face.
  FT_Face regular=manager.GetFace(TEXT_STYLE_REGULAR);
  manager.ClearCache(TEXT_STYLE_REGULAR);
  reads=0;
  manager.GetGlyph('A',flags,regular);
  assert(reads>0);
  Unobserve(regular);
  manager.SetFontFile("LiberationSerif-Bold.ttf",TEXT_STYLE_REGULAR);
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
  reads=0;
  manager.GetGlyph(544,flags,regular);
  assert(reads==0);
  manager.GetGlyph(33,flags,regular);
  assert(reads>0);
  // A real fallback remains native at its own active size across primary
  // size changes, and replacing/unloading it retires its cached bitmaps.
  assert(manager.SetFallbackFile(0,fallback));
  FT_Face oracle;
  assert(!FT_New_Face(library,fallback,0,&oracle));
  assert(!FT_Set_Pixel_Sizes(oracle,0,10));
  assert(!FT_Load_Char(oracle,0x0531,flags));
  for (u8 px : {10,18,10}) {
    manager.SetPixelSize(px);
    ExpectGlyph(manager.GetGlyph(0x0531,flags,regular),oracle->glyph);
  }
  assert(manager.SetFallbackFile(0,fallback));
  ExpectGlyph(manager.GetGlyph(0x0531,flags,regular),oracle->glyph);
  manager.UnloadFallbackFonts();
  FT_Done_Face(oracle);
  for (u8 style : {TEXT_STYLE_REGULAR, TEXT_STYLE_BOLD, TEXT_STYLE_BROWSER})
    Unobserve(manager.GetFace(style));
  FT_Done_FreeType(library);
  puts("PASS: glyph pixels, size reuse, shared LRU budget, face/fallback retirement");
}
