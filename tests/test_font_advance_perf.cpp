#include "ui/text.h"
#include "ui/font_constants.h"
#include "shared/debug_log.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// FontManager and FreeType are real. Text supplies configuration/reporter only;
// rendering and UTF-8 services must not be called by these advance scenarios.
Text::Text() : reporter_(nullptr), fm(nullptr), tr(nullptr) {
  pixelsize = 12;
  margin.left = margin.right = margin.top = margin.bottom = 0;
}
Text::~Text() {}
void Text::SetReporter(IStatusReporter *reporter) { reporter_ = reporter; }
int Text::GetStyle() { std::abort(); }
u8 Text::GetCharCode(const char *, size_t, u32 *) { std::abort(); }
void TextRenderer::SetHit(bool) { std::abort(); }
int halt(Text *, const char *, int) { std::abort(); }

struct Reporter : IStatusReporter {
  std::vector<std::string> lines;
  void PrintStatus(const char *s) { lines.push_back(s); }
  void PrintStatus(std::string s) { lines.push_back(s); }
};
static bool Contains(const Reporter &r, const std::string &text) {
  for (const auto &line : r.lines)
    if (line.find(text) != std::string::npos) return true;
  return false;
}
static u8 NativeAdvance(FT_Face face, u32 codepoint) {
  assert(FT_Get_Char_Index(face, codepoint) != 0);
  assert(FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) == 0);
  return static_cast<u8>(face->glyph->advance.x >> 6);
}
#ifdef DSLIBRIS_DEBUG
static std::string FaceSize(FT_Face face, unsigned px, unsigned entries) {
  char line[128];
  std::snprintf(line, sizeof(line), "ADVANCE face=%p px=%u entries=%u",
                static_cast<void *>(face), px, entries);
  return line;
}
#endif
int main() {
  const char *font_dir = std::getenv("TEST_ADVANCE_FONT_DIR");
  const char *fallback_file = std::getenv("TEST_ADVANCE_FALLBACK");
  assert(font_dir && fallback_file);
  Reporter reporter;
  Text text;
  text.SetReporter(&reporter);
  FontManager manager(&text);
  manager.SetFontDir(font_dir);
  assert(manager.Init() == 0);
  manager.UnloadFallbackFonts();
  assert(manager.SetFallbackFile(0, fallback_file));
  FT_Face primary = manager.GetFace(TEXT_STYLE_REGULAR);
  FT_Face bold = manager.GetFace(TEXT_STYLE_BOLD);
  assert(primary && bold);
  const u32 armenian = 0x0531;
  assert(FT_Get_Char_Index(primary, armenian) == 0);
  assert(FT_Get_Char_Index(bold, armenian) == 0);
  FT_Library oracle_library;
  FT_Face oracle_fallback;
  assert(FT_Init_FreeType(&oracle_library) == 0);
  assert(FT_New_Face(oracle_library, fallback_file, 0, &oracle_fallback) == 0);
  assert(FT_Set_Pixel_Sizes(oracle_fallback, 0, 12) == 0);
  const u8 latin12 = NativeAdvance(primary, 'A');
  const u8 fallback12 = NativeAdvance(oracle_fallback, armenian);
  manager.ClearCache();
  reporter.lines.clear();
  assert(manager.GetAdvance('A', primary) == latin12);
  assert(manager.GetAdvance('A', primary) == latin12);
  assert(manager.GetAdvance(armenian, primary) == fallback12);
  assert(manager.GetAdvance(armenian, primary) == fallback12);
  assert(manager.GetAdvance(0x10ffff, primary) == 0);
  assert(manager.GetAdvance(armenian, bold) == fallback12);
  manager.SetPixelSize(18);
  const u8 latin18 = NativeAdvance(primary, 'A');
  assert(manager.GetAdvance('A', primary) == latin18);
  assert(manager.GetAdvance('A', primary) == latin18);
  assert(!Contains(reporter, "ADVANCE")); // No per-glyph or size-change logs.
  manager.ClearCache();
#ifdef DSLIBRIS_DEBUG
  assert(Contains(reporter, "ADVANCE calls=8 hits=3 misses=5 loads=3"));
  assert(Contains(reporter, "ADVANCE fallback searches=3 hits=1 unresolved=1 ftc=0"));
  assert(Contains(reporter, FaceSize(primary, 12, 2)));
  assert(Contains(reporter, FaceSize(primary, 18, 1)));
  assert(Contains(reporter, FaceSize(bold, 12, 1)));
  unsigned faces = 0, entries = 0;
  for (const auto &line : reporter.lines) {
    if (line.find("ADVANCE face=") != std::string::npos &&
        line.find("value_bytes=") != std::string::npos) {
      ++faces;
      const size_t field = line.find("entries=");
      assert(field != std::string::npos);
      entries += static_cast<unsigned>(std::strtoul(line.c_str() + field + 8, nullptr, 10));
    }
  }
  assert(faces == 3 && entries == 5); // Includes the deduplicated fallback face.
#else
  assert(!Contains(reporter, "ADVANCE"));
#endif
  reporter.lines.clear();
  assert(manager.GetAdvance('A', primary) == latin18);
  manager.ClearCache();
#ifdef DSLIBRIS_DEBUG
  assert(Contains(reporter, "ADVANCE calls=1 hits=0 misses=1 loads=1"));
  assert(Contains(reporter, FaceSize(primary, 18, 1)));
  assert(!Contains(reporter, "px=12 entries="));
#endif
  FT_Done_Face(oracle_fallback);
  FT_Done_FreeType(oracle_library);
  puts("PASS: real FreeType advances, fallback, cache statistics and reset");
}
