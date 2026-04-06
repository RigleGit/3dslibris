#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "shared/text_layout_utils.h"

// Arabic contextual shaping: maps base Arabic codepoints (U+0622-U+064A)
// to their presentation forms (U+FE70-U+FEFF) based on joining context.
// Integrates after ShapeTextRunUtf8() and before AnalyzeBidiRuns().
namespace text_arabic_shaping {

// Returns true if the codepoint array contains any base Arabic letter
// (U+0600-U+06FF). Used as a fast-path guard before full shaping.
bool ContainsArabic(const uint32_t *cps, size_t count);

// Apply Arabic contextual shaping in-place to a ShapedGlyph vector.
// Replaces base Arabic codepoints (U+0622-U+064A) with their contextual
// presentation forms based on joining neighbours.
// Glyphs whose codepoint changes are remeasured via measure_fn.
// Returns true if any glyph was changed.
bool ApplyContextualShaping(std::vector<text_layout_utils::ShapedGlyph> *glyphs,
                             text_layout_utils::MeasureCodepointFn measure_fn,
                             void *measure_ctx);

} // namespace text_arabic_shaping
