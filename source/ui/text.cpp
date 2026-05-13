#include "ui/text.h"

#include <string.h>
#include <vector>

#include "shared/debug_log.h"
#include "ui/screen_constants.h"
#include "shared/text_layout_utils.h"
#include "shared/text_unicode_utils.h"

#define PIXELSIZE 12

namespace {

struct TextAdvanceContext {
  Text *text;
  u8 style;
};

static int MeasureTextAdvance(uint32_t codepoint, void *ctx) {
  TextAdvanceContext *measure = (TextAdvanceContext *)ctx;
  if (!measure || !measure->text)
    return 0;
  return measure->text->GetAdvance(codepoint, measure->style);
}

}

Text::Text() : reporter_(nullptr), fm(nullptr), tr(nullptr) {
  display.height = PAGE_HEIGHT;
  display.width = PAGE_WIDTH;

  int bufsize = PAGE_HEIGHT * PAGE_HEIGHT;
  screenleft = new u16[bufsize];
  screenright = new u16[bufsize];
  offscreen = new u16[bufsize];
  memset(screenleft, 0xFF, bufsize * sizeof(u16));
  memset(screenright, 0xFF, bufsize * sizeof(u16));
  memset(offscreen, 0xFF, bufsize * sizeof(u16));

  margin.left = MARGINLEFT;
  margin.right = MARGINRIGHT;
  margin.top = MARGINTOP;
  margin.bottom = MARGINBOTTOM;
  bgcolor.r = 15;
  bgcolor.g = 15;
  bgcolor.b = 15;
  fgcolor = 0;
  usefgcolor = false;
  usebgcolor = false;
  linespacing = 1;

  linebegan = false;
  bold = false;
  italic = false;
  pixelsize = PIXELSIZE;
  screen = screenleft;
  screenleft_dirty = true;
  screenright_dirty = true;
  screenleft_cache_generation = 1;
  screenright_cache_generation = 1;
  screenleft_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, display.width,
      framebuffer_blit_utils::LogicalTextScreenHeight(true));
  screenright_dirty_rect = framebuffer_blit_utils::MakeDirtyRect(
      0, 0, display.width,
      framebuffer_blit_utils::LogicalTextScreenHeight(false));

  fm = new FontManager(this);
  tr = new TextRenderer(this);

  tr->ClearScreen(offscreen, 255, 255, 255);
}

Text::~Text() {
  delete tr;
  delete fm;
  delete[] offscreen;
  delete[] screenleft;
  delete[] screenright;
}

int Text::Init() { return fm->Init(); }

void Text::SetReporter(IStatusReporter *reporter) { reporter_ = reporter; }
IStatusReporter *Text::GetReporter() const { return reporter_; }
void Text::SetFontDir(const std::string &dir) { fm->SetFontDir(dir); }

void Text::InitPen() { tr->InitPen(); }

FT_Face Text::GetFace() { return fm->GetFace((u8)GetStyle()); }

FT_Face Text::GetFace(u8 astyle) { return fm->GetFace(astyle); }

std::string Text::GetFontFile(u8 style) { return fm->GetFontFile(style); }

std::string Text::GetFallbackFontFile(int index) const {
  if (!fm)
    return std::string();
  return fm->GetFallbackFile(index);
}

std::string Text::GetFontName(u8 style) { return fm->GetFontName(style); }

bool Text::GetFontName(std::string &s) { return fm->GetFontName(s); }

u8 Text::GetHeight() { return fm ? fm->GetHeight() : PIXELSIZE; }

void Text::GetPen(u16 *x, u16 *y) { tr->GetPen(x, y); }

void Text::GetPen(u16 &x, u16 &y) { tr->GetPen(x, y); }

u16 Text::GetPenX() { return tr->GetPenX(); }

u16 Text::GetPenY() { return tr->GetPenY(); }

u8 Text::GetPixelSize() { return (u8)fm->GetPixelSize(); }

u16 *Text::GetScreen() { return tr->GetScreen(); }

int Text::GetStringAdvance(const char *s) {
  if (!s)
    return 0;

  const size_t len = strlen(s);
  int advance = 0;
  for (size_t offset = 0; offset < len;) {
    u32 ucs = 0;
    u8 bytes = GetCharCode(s + offset, len - offset, &ucs);
    if (!bytes) {
      bytes = 1;
      ucs = '?';
    }
    advance += GetAdvance(ucs);
    offset += bytes;
  }
  return advance;
}

u8 Text::GetStringWidth(const char *txt, u8 style) {
  return GetStringWidth(txt, GetFace(style));
}

int Text::GetStyle() { return tr->GetStyle(); }

void Text::SetColorMode(int mode) {
  tr->SetColorMode(mode);
}

int Text::GetColorMode() { return tr->GetColorMode(); }

bool Text::IsAutoWrapEnabled() const { return tr->IsAutoWrapEnabled(); }

void Text::SetAutoWrapEnabled(bool enabled) {
  tr->SetAutoWrapEnabled(enabled);
}

bool Text::IsClipToContentEnabled() const {
  return tr->IsClipToContentEnabled();
}

void Text::SetClipToContentEnabled(bool enabled) {
  tr->SetClipToContentEnabled(enabled);
}

void Text::SetScriptScale(float s) { if (tr) tr->SetScriptScale(s); }
float Text::GetScriptScale() const { return tr ? tr->GetScriptScale() : 1.0f; }

void Text::SetOrientation(bool turned_right) { tr->SetOrientation(turned_right); }

bool Text::GetOrientation() const { return tr->GetOrientation(); }

void Text::SetPen(u16 x, u16 y) { tr->SetPen(x, y); }

void Text::SetPixelSize(u8 size) { fm->SetPixelSize(size); }

void Text::SetTextColorOverride(u16 color) { tr->SetTextColorOverride(color); }

void Text::ClearTextColorOverride() { tr->ClearTextColorOverride(); }

void Text::SetFace(u8 astyle) { tr->SetStyle((int)astyle); }

void Text::SetFontFile(const char *path, u8 style) { fm->SetFontFile(path, style); }

bool Text::SetFallbackFontFile(int index, const char *path) {
  if (!fm)
    return false;
  return fm->SetFallbackFile(index, path);
}

void Text::ClearFallbackFonts() {
  if (!fm)
    return;
  fm->UnloadFallbackFonts();
}

void Text::AutoLoadFallbackFonts() {
  if (!fm)
    return;
  fm->AutoLoadCjkFallbackFonts();
}

void Text::SetScreen(u16 *s) { tr->SetScreen(s); }

void Text::SetStyle(int astyle) { tr->SetStyle(astyle); }

void Text::MarkScreenDirty(u16 *target) { tr->MarkScreenDirty(target); }

void Text::MarkScreenDirtyRect(u16 *target, int x0, int y0, int x1, int y1) {
  tr->MarkScreenDirtyRect(target, x0, y0, x1, y1);
}

void Text::MarkCurrentScreenDirty() { tr->MarkCurrentScreenDirty(); }

void Text::MarkCurrentScreenDirtyRect(int x0, int y0, int x1, int y1) {
  tr->MarkCurrentScreenDirtyRect(x0, y0, x1, y1);
}

void Text::MarkAllScreensDirty() { tr->MarkAllScreensDirty(); }

bool Text::HasDirtyScreens() const { return tr->HasDirtyScreens(); }

void Text::ClearCache() { fm->ClearCache(); }

void Text::ClearCache(u8 style) { fm->ClearCache(style); }

void Text::ClearRect(u16 xl, u16 yl, u16 xh, u16 yh) {
  tr->ClearRect(xl, yl, xh, yh);
}

void Text::FillRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  tr->FillRect(xl, yl, xh, yh, color);
}

void Text::DrawRect(u16 xl, u16 yl, u16 xh, u16 yh, u16 color) {
  tr->DrawRect(xl, yl, xh, yh, color);
}

u16 Text::GetFgColor() { return tr->GetFgColor(); }

u16 Text::GetBgColor() { return tr->GetBgColor(); }

void Text::ClearScreen() { tr->ClearScreen(); }

void Text::ClearScreen(u16 *s, u8 r, u8 g, u8 b) { tr->ClearScreen(s, r, g, b); }

void Text::CopyScreen(u16 *src, u16 *dst) { tr->CopyScreen(src, dst); }

bool Text::BlitToFramebuffer() { return tr->BlitToFramebuffer(); }

void Text::PrintChar(u32 ucs) { tr->PrintChar(ucs); }

void Text::PrintChar(u32 ucs, u8 style) { tr->PrintChar(ucs, style); }

bool Text::PrintNewLine(void) { return tr->PrintNewLine(); }

void Text::PrintString(const char *string) { tr->PrintString(string); }

void Text::PrintString(const char *string, u8 style) {
  tr->PrintString(string, style);
}

void Text::PrintSplash(u16 *s) { tr->PrintSplash(s); }

u8 Text::GetCharCode(const char *utf8, u32 *ucs) {
  if (!utf8 || !ucs || !utf8[0])
    return 0;
  return GetCharCode(utf8, strlen(utf8), ucs);
}

u8 Text::GetCharCode(const char *utf8, size_t remaining, u32 *ucs) {
  if (!utf8 || !ucs || !utf8[0] || remaining == 0)
    return 0;
  return (u8)text_unicode_utils::DecodeNextDisplayCodepoint(utf8, remaining,
                                                             ucs);
}

u8 Text::GetCharCountInsideWidth(const char *txt, u8 style, u8 pixels) {
  if (!txt || !*txt)
    return 0;

  TextAdvanceContext measure{this, style};
  std::vector<text_layout_utils::ShapedGlyph> run;
  if (!text_layout_utils::ShapeTextRunUtf8(txt, strlen(txt), NULL,
                                           MeasureTextAdvance, &measure, &run))
    return 0;

  u8 count = 0;
  u16 width = 0;
  u16 cluster_width = 0;
  bool have_cluster = false;
  for (size_t i = 0; i < run.size(); i++) {
    if (run[i].text.grapheme_start) {
      if (have_cluster) {
        if ((u16)(width + cluster_width) > pixels)
          return count;
        width = (u16)(width + cluster_width);
        count++;
      }
      cluster_width = 0;
      have_cluster = true;
    }
    cluster_width = (u16)(cluster_width + run[i].advance);
  }

  if (have_cluster && (u16)(width + cluster_width) <= pixels)
    count++;
  return count;
}

int Text::CacheGlyph(u32 ucs, FT_Face face) { return fm->CacheGlyph(ucs, face); }

void Text::ClearCache(FT_Face face) { fm->ClearCache(face); }

FT_GlyphSlot Text::GetGlyph(u32 ucs, int flags, FT_Face face) {
  return fm->GetGlyph(ucs, flags, face);
}

FT_Error Text::GetGlyphBitmap(u32 ucs, FTC_SBit *asbit, FTC_Node *anode) {
  return fm->GetGlyphBitmap(ucs, asbit, anode);
}

FT_UInt Text::GetGlyphIndex(u32 ucs) { return fm->GetGlyphIndex(ucs); }

u8 Text::GetAdvance(u32 ucs, FT_Face face) { return fm ? fm->GetAdvance(ucs, face) : 0; }

u8 Text::GetStringWidth(const char *txt, FT_Face face) {
  return fm->GetStringWidth(txt, face);
}

void Text::PrintChar(u32 ucs, FT_Face face) { tr->PrintChar(ucs, face); }

void Text::PrintString(const char *string, FT_Face face) {
  tr->PrintString(string, face);
}

void Text::ReportFace(FT_Face face) { fm->ReportFace(face); }
