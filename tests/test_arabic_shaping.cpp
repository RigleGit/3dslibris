#include "shared/text_arabic_shaping.h"
#include "shared/text_layout_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool v) {
  if (!v) Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool v) {
  if (v) Fail(std::string(label) + ": expected false");
}

void ExpectEqU32(const char *label, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: expected U+%04X, got U+%04X",
             label, expected, actual);
    Fail(buf);
  }
}

// Measure callback: returns 1 for every codepoint (unit-width glyphs)
static int MeasureUnit(uint32_t /*cp*/, void * /*ctx*/) { return 1; }

// Build a ShapedGlyph vector from a raw codepoint array using MeasureUnit.
static std::vector<text_layout_utils::ShapedGlyph>
MakeGlyphs(const uint32_t *cps, size_t count) {
  std::vector<text_layout_utils::ShapedGlyph> glyphs(count);
  for (size_t i = 0; i < count; i++) {
    glyphs[i].text.codepoint = cps[i];
    glyphs[i].advance = 1;
    glyphs[i].bidi_level = 0;
  }
  return glyphs;
}

// ── ContainsArabic ────────────────────────────────────────────────────────────

void TestContainsArabic_LatinOnly() {
  const uint32_t cps[] = {'H', 'e', 'l', 'l', 'o'};
  ExpectFalse("Latin-only ContainsArabic",
              text_arabic_shaping::ContainsArabic(cps, 5));
}

void TestContainsArabic_ArabicLetter() {
  const uint32_t cps[] = {0x0643}; // KAF
  ExpectTrue("Arabic letter ContainsArabic",
             text_arabic_shaping::ContainsArabic(cps, 1));
}

void TestContainsArabic_PresentationFormsOnly() {
  // Already-shaped forms: NOT in U+0600-U+06FF
  const uint32_t cps[] = {0xFEDB}; // KAF initial form
  ExpectFalse("Presentation form ContainsArabic",
              text_arabic_shaping::ContainsArabic(cps, 1));
}

void TestContainsArabic_Mixed() {
  const uint32_t cps[] = {'A', 0x0628, 'B'}; // BA in the middle
  ExpectTrue("Mixed ContainsArabic",
             text_arabic_shaping::ContainsArabic(cps, 3));
}

// ── Single-letter isolation ───────────────────────────────────────────────────

void TestIsolated_Alef() {
  // U+0627 ALEF alone → U+FE8D isolated form
  const uint32_t cps[] = {0x0627};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 1);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("Alef isolated", g[0].text.codepoint, 0xFE8D);
}

void TestIsolated_Ba() {
  // U+0628 BA alone → U+FE8F isolated form
  const uint32_t cps[] = {0x0628};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 1);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("Ba isolated", g[0].text.codepoint, 0xFE8F);
}

void TestIsolated_Kaf() {
  const uint32_t cps[] = {0x0643};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 1);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("Kaf isolated", g[0].text.codepoint, 0xFED9);
}

// ── Two-letter combinations ──────────────────────────────────────────────────

void TestFinalInitial_BaBa() {
  // BA + BA in logical RTL order:
  //   index 0 (visual right) = INITIAL form
  //   index 1 (visual left)  = FINAL   form
  const uint32_t cps[] = {0x0628, 0x0628};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 2);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("BaBa[0] initial", g[0].text.codepoint, 0xFE91); // initial
  ExpectEqU32("BaBa[1] final",   g[1].text.codepoint, 0xFE90); // final
}

void TestFinalOnly_BaAlef() {
  // BA(0) + ALEF(1):
  //   BA at 0: connects left (ALEF is R-type, can present right side) → INITIAL
  //   ALEF at 1: connects right (BA at 0 is D-type) → FINAL
  const uint32_t cps[] = {0x0628, 0x0627};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 2);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("BaAlef[0] initial", g[0].text.codepoint, 0xFE91); // BA initial
  ExpectEqU32("BaAlef[1] final",   g[1].text.codepoint, 0xFE8E); // ALEF final
}

void TestAlefBa_SplitWord() {
  // ALEF(0) + BA(1):
  //   ALEF at 0: right neighbour = nothing → ISOLATED
  //   BA at 1: right neighbour = ALEF (R-type, cannot connect left) → ISOLATED
  const uint32_t cps[] = {0x0627, 0x0628};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 2);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("AlefBa[0] Alef isolated", g[0].text.codepoint, 0xFE8D);
  ExpectEqU32("AlefBa[1] Ba isolated",   g[1].text.codepoint, 0xFE8F);
}

// ── Three-letter word (medial form) ──────────────────────────────────────────

void TestMedial_KafTaLam() {
  // KAF(0) + TA(1) + LAM(2) — all D-type
  const uint32_t cps[] = {0x0643, 0x062A, 0x0644};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 3);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("KafTaLam[0] Kaf initial", g[0].text.codepoint, 0xFEDB); // KAF initial
  ExpectEqU32("KafTaLam[1] Ta medial",   g[1].text.codepoint, 0xFE98); // TA  medial
  ExpectEqU32("KafTaLam[2] Lam final",   g[2].text.codepoint, 0xFEDE); // LAM final
}

void TestMedial_SinAlefBa() {
  // SIN(0) + ALEF(1) + BA(2):
  //   SIN at 0: connects left (ALEF R-type can present right side) → INITIAL
  //   ALEF at 1: connects right (SIN D-type) → FINAL; breaks chain
  //   BA at 2: right neighbour = ALEF (R-type, cannot join left) → ISOLATED
  const uint32_t cps[] = {0x0633, 0x0627, 0x0628};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 3);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("SinAlefBa[0] Sin initial", g[0].text.codepoint, 0xFEB3); // SIN initial
  ExpectEqU32("SinAlefBa[1] Alef final",  g[1].text.codepoint, 0xFE8E); // ALEF final
  ExpectEqU32("SinAlefBa[2] Ba isolated", g[2].text.codepoint, 0xFE8F); // BA isolated
}

// ── Diacritics (harakat) are transparent ─────────────────────────────────────

void TestDiacriticTransparent() {
  // BA(0) + FATHA-diacritic(1) + BA(2):
  // The diacritic at index 1 is transparent; BA(0) and BA(2) should still join.
  // BA(0): right neighbour = none → left neighbour across fatha = BA(2) D-type → INITIAL
  // BA(2): right neighbour across fatha = BA(0) D-type → left neighbour = none → FINAL
  const uint32_t cps[] = {0x0628, 0x064E /*FATHA*/, 0x0628};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 3);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("DiacriticTransparent[0] Ba initial", g[0].text.codepoint, 0xFE91);
  ExpectEqU32("DiacriticTransparent[2] Ba final",   g[2].text.codepoint, 0xFE90);
}

// ── Latin pass-through ────────────────────────────────────────────────────────

void TestLatinUnchanged() {
  const uint32_t cps[] = {'H', 'e', 'l', 'l', 'o'};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 5);
  bool changed = text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectFalse("Latin unchanged: return false", changed);
  ExpectEqU32("Latin[0]", g[0].text.codepoint, 'H');
  ExpectEqU32("Latin[4]", g[4].text.codepoint, 'o');
}

// ── Mixed Arabic + Latin ──────────────────────────────────────────────────────

void TestMixedArabicLatin() {
  // Latin 'A'(0) + KAF(1) + LAM(2) + Latin 'B'(3)
  // Latin breaks the joining context:
  //   KAF(1): right neighbour = Latin 'A' (JOIN_NONE) → left neighbour = LAM D-type → INITIAL
  //   LAM(2): right neighbour = KAF D-type → left neighbour = Latin 'B' (JOIN_NONE) → FINAL
  const uint32_t cps[] = {'A', 0x0643, 0x0644, 'B'};
  std::vector<text_layout_utils::ShapedGlyph> g = MakeGlyphs(cps, 4);
  text_arabic_shaping::ApplyContextualShaping(&g, MeasureUnit, NULL);
  ExpectEqU32("Mixed[0] A unchanged", g[0].text.codepoint, 'A');
  ExpectEqU32("Mixed[1] Kaf initial", g[1].text.codepoint, 0xFEDB);
  ExpectEqU32("Mixed[2] Lam final",   g[2].text.codepoint, 0xFEDE);
  ExpectEqU32("Mixed[3] B unchanged", g[3].text.codepoint, 'B');
}

// ── Advance remeasure ─────────────────────────────────────────────────────────

void TestAdvanceRemeasured() {
  // Measure callback that returns the low byte of the codepoint as width.
  static auto measure_lo = [](uint32_t cp, void *) -> int {
    return (int)(cp & 0xFF);
  };
  text_layout_utils::MeasureCodepointFn fn = measure_lo;

  const uint32_t cps[] = {0x0643}; // KAF → isolated 0xFED9; 0xD9 = 217
  std::vector<text_layout_utils::ShapedGlyph> g(1);
  g[0].text.codepoint = 0x0643;
  g[0].advance = (int)(0x0643 & 0xFF); // original KAF advance
  g[0].bidi_level = 0;

  text_arabic_shaping::ApplyContextualShaping(&g, fn, NULL);
  ExpectEqU32("Remeasure: shaped codepoint", g[0].text.codepoint, 0xFED9);
  if (g[0].advance != (int)(0xFED9 & 0xFF))
    Fail("Remeasure: advance not updated after shaping");
}

} // namespace

int main() {
  TestContainsArabic_LatinOnly();
  TestContainsArabic_ArabicLetter();
  TestContainsArabic_PresentationFormsOnly();
  TestContainsArabic_Mixed();

  TestIsolated_Alef();
  TestIsolated_Ba();
  TestIsolated_Kaf();

  TestFinalInitial_BaBa();
  TestFinalOnly_BaAlef();
  TestAlefBa_SplitWord();

  TestMedial_KafTaLam();
  TestMedial_SinAlefBa();

  TestDiacriticTransparent();
  TestLatinUnchanged();
  TestMixedArabicLatin();
  TestAdvanceRemeasured();

  printf("All arabic_shaping tests passed.\n");
  return 0;
}
