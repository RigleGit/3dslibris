#include "shared/text_arabic_shaping.h"

#include <cstddef>

namespace text_arabic_shaping {

namespace {

enum JoinType {
  JOIN_NONE        = 0, // non-joining (Latin, numerals, punctuation, …)
  JOIN_RIGHT       = 1, // right-joining only (Alef, Dal, Ra, Waw, …)
  JOIN_DUAL        = 2, // dual-joining (Ba, Ta, Sin, Kaf, Lam, …)
  JOIN_TRANSPARENT = 3  // diacritics — do not break joining chain
};

struct ArabicLetterEntry {
  uint32_t base;
  JoinType join_type;
  uint32_t isolated;
  uint32_t final_;   // 'final' is reserved in some C++ dialects
  uint32_t initial;  // 0 for R-type letters (no initial/medial forms)
  uint32_t medial;   // 0 for R-type letters
};

// Presentation forms from Unicode Arabic Presentation Forms-B (U+FE70-U+FEFF)
// and a small number from Forms-A (U+FB50-U+FDFF) for completeness.
// Order must be kept stable — binary search is not used given the small size.
static const ArabicLetterEntry kTable[] = {
  // R-type: isolated + final only
  {0x0622, JOIN_RIGHT, 0xFE81, 0xFE82, 0x0000, 0x0000}, // ALEF WITH MADDA
  {0x0623, JOIN_RIGHT, 0xFE83, 0xFE84, 0x0000, 0x0000}, // ALEF WITH HAMZA ABOVE
  {0x0624, JOIN_RIGHT, 0xFE85, 0xFE86, 0x0000, 0x0000}, // WAW WITH HAMZA ABOVE
  {0x0625, JOIN_RIGHT, 0xFE87, 0xFE88, 0x0000, 0x0000}, // ALEF WITH HAMZA BELOW
  {0x0627, JOIN_RIGHT, 0xFE8D, 0xFE8E, 0x0000, 0x0000}, // ALEF
  {0x0629, JOIN_RIGHT, 0xFE93, 0xFE94, 0x0000, 0x0000}, // TEH MARBUTA
  {0x062F, JOIN_RIGHT, 0xFEA9, 0xFEAA, 0x0000, 0x0000}, // DAL
  {0x0630, JOIN_RIGHT, 0xFEAB, 0xFEAC, 0x0000, 0x0000}, // THAL
  {0x0631, JOIN_RIGHT, 0xFEAD, 0xFEAE, 0x0000, 0x0000}, // REH
  {0x0632, JOIN_RIGHT, 0xFEAF, 0xFEB0, 0x0000, 0x0000}, // ZAIN
  {0x0648, JOIN_RIGHT, 0xFEED, 0xFEEE, 0x0000, 0x0000}, // WAW
  {0x0649, JOIN_RIGHT, 0xFEEF, 0xFEF0, 0x0000, 0x0000}, // ALEF MAQSURA

  // D-type: all four forms
  {0x0626, JOIN_DUAL, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // YEH WITH HAMZA ABOVE
  {0x0628, JOIN_DUAL, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // BA
  {0x062A, JOIN_DUAL, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, // TA
  {0x062B, JOIN_DUAL, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // THEH
  {0x062C, JOIN_DUAL, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // JEEM
  {0x062D, JOIN_DUAL, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // HAH
  {0x062E, JOIN_DUAL, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // KHAH
  {0x0633, JOIN_DUAL, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // SEEN
  {0x0634, JOIN_DUAL, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // SHEEN
  {0x0635, JOIN_DUAL, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // SAD
  {0x0636, JOIN_DUAL, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // DAD
  {0x0637, JOIN_DUAL, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // TAH
  {0x0638, JOIN_DUAL, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // ZAH
  {0x0639, JOIN_DUAL, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // AIN
  {0x063A, JOIN_DUAL, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, // GHAIN
  {0x0641, JOIN_DUAL, 0xFED1, 0xFED2, 0xFED3, 0xFED4}, // FA
  {0x0642, JOIN_DUAL, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, // QAF
  {0x0643, JOIN_DUAL, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // KAF
  {0x0644, JOIN_DUAL, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // LAM
  {0x0645, JOIN_DUAL, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // MEEM
  {0x0646, JOIN_DUAL, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // NOON
  {0x0647, JOIN_DUAL, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // HEH
  {0x064A, JOIN_DUAL, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // YEH
};

static const size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

static JoinType GetJoinType(uint32_t cp) {
  // Tatweel (stretching connector): dual-joining, no shaped form
  if (cp == 0x0640) return JOIN_DUAL;
  // Arabic diacritics (harakat) U+064B-U+065F: transparent
  if (cp >= 0x064B && cp <= 0x065F) return JOIN_TRANSPARENT;
  // Arabic combining signs U+0610-U+061A: transparent
  if (cp >= 0x0610 && cp <= 0x061A) return JOIN_TRANSPARENT;
  // General combining marks (non-spacing): transparent
  if (cp >= 0x0300 && cp <= 0x036F) return JOIN_TRANSPARENT;

  for (size_t i = 0; i < kTableSize; i++) {
    if (kTable[i].base == cp)
      return kTable[i].join_type;
  }
  return JOIN_NONE;
}

static const ArabicLetterEntry *GetEntry(uint32_t cp) {
  for (size_t i = 0; i < kTableSize; i++) {
    if (kTable[i].base == cp)
      return &kTable[i];
  }
  return NULL;
}

// Right neighbour = lower logical index (visually to the right in RTL).
// Skip transparent characters. Returns JOIN_NONE if none found.
static JoinType RightNeighbourType(const uint32_t *cps, size_t pos) {
  if (pos == 0) return JOIN_NONE;
  size_t j = pos - 1;
  while (true) {
    JoinType jt = GetJoinType(cps[j]);
    if (jt != JOIN_TRANSPARENT) return jt;
    if (j == 0) break;
    j--;
  }
  return JOIN_NONE;
}

// Left neighbour = higher logical index (visually to the left in RTL).
static JoinType LeftNeighbourType(const uint32_t *cps, size_t count, size_t pos) {
  for (size_t j = pos + 1; j < count; j++) {
    JoinType jt = GetJoinType(cps[j]);
    if (jt != JOIN_TRANSPARENT) return jt;
  }
  return JOIN_NONE;
}

} // namespace

bool ContainsArabic(const uint32_t *cps, size_t count) {
  if (!cps) return false;
  for (size_t i = 0; i < count; i++) {
    if (cps[i] >= 0x0600 && cps[i] <= 0x06FF)
      return true;
  }
  return false;
}

bool ApplyContextualShaping(std::vector<text_layout_utils::ShapedGlyph> *glyphs,
                             text_layout_utils::MeasureCodepointFn measure_fn,
                             void *measure_ctx) {
  if (!glyphs || glyphs->empty()) return false;

  const size_t n = glyphs->size();

  // Snapshot base codepoints — we use these for all neighbour lookups so that
  // the order in which we iterate does not affect the result.
  std::vector<uint32_t> cps(n);
  for (size_t i = 0; i < n; i++)
    cps[i] = (*glyphs)[i].text.codepoint;

  if (!ContainsArabic(cps.data(), n)) return false;

  bool changed = false;

  for (size_t i = 0; i < n; i++) {
    const ArabicLetterEntry *entry = GetEntry(cps[i]);
    if (!entry) continue;

    uint32_t new_cp;

    if (entry->join_type == JOIN_DUAL) {
      // A dual-joining letter connects:
      //   - RIGHT (to i-1) if i-1 is dual-joining (only D-type can join on their left)
      //   - LEFT  (to i+1) if i+1 is dual- or right-joining (D or R type can present
      //     a right side to this letter's left)
      const JoinType rn = RightNeighbourType(cps.data(), i);
      const JoinType ln = LeftNeighbourType(cps.data(), n, i);
      const bool cr = (rn == JOIN_DUAL);
      const bool cl = (ln == JOIN_DUAL || ln == JOIN_RIGHT);

      if (cr && cl)      new_cp = entry->medial;
      else if (cr)       new_cp = entry->final_;
      else if (cl)       new_cp = entry->initial;
      else               new_cp = entry->isolated;
    } else {
      // R-type: connects RIGHT only (to i-1 if i-1 is dual-joining)
      const JoinType rn = RightNeighbourType(cps.data(), i);
      new_cp = (rn == JOIN_DUAL) ? entry->final_ : entry->isolated;
    }

    if (new_cp != cps[i]) {
      (*glyphs)[i].text.codepoint = new_cp;
      if (measure_fn)
        (*glyphs)[i].advance = measure_fn(new_cp, measure_ctx);
      changed = true;
    }
  }

  return changed;
}

} // namespace text_arabic_shaping
