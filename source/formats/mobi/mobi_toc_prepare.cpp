/*
    3dslibris - mobi_toc_prepare.cpp
*/

#include "formats/mobi/mobi_toc_prepare.h"

#include "formats/mobi/mobi_toc_resolver.h"

namespace mobi_toc_prepare {

static mobi_toc_resolver::PrepareCallbacks BuildCallbacks(
    const StructuredCallbacks &structured_callbacks,
    const InlineTitleCallbacks &inline_title_callbacks) {
  mobi_toc_resolver::PrepareCallbacks callbacks;
  callbacks.structured.decode_bytes_to_utf8 =
      structured_callbacks.decode_bytes_to_utf8;
  callbacks.structured.normalize_title = structured_callbacks.normalize_title;
  callbacks.structured.reject_title = structured_callbacks.reject_title;
  callbacks.inline_title.looks_like_structured_title =
      inline_title_callbacks.looks_like_structured_title;
  callbacks.inline_title.fold_latin_for_match =
      inline_title_callbacks.fold_latin_for_match;
  callbacks.inline_title.count_ascii_words =
      inline_title_callbacks.count_ascii_words;
  callbacks.inline_title.is_mostly_digits_or_punctuation =
      inline_title_callbacks.is_mostly_digits_or_punctuation;
  callbacks.decode_bytes_to_utf8 = structured_callbacks.decode_bytes_to_utf8;
  return callbacks;
}

bool Prepare(const std::string &raw, const std::vector<u32> &offsets,
             u32 ncx_index, u32 encoding, const std::string *markup_utf8,
             u32 text_len, const StructuredCallbacks &structured_callbacks,
             const InlineTitleCallbacks &inline_title_callbacks,
             std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
                 *out,
             bool *structured_from_filepos, IStatusReporter *reporter) {
  const mobi_toc_resolver::PrepareCallbacks callbacks =
      BuildCallbacks(structured_callbacks, inline_title_callbacks);
  return mobi_toc_resolver::PrepareStructuredToc(
      raw, offsets, ncx_index, encoding, markup_utf8, text_len, callbacks, out,
      structured_from_filepos, reporter);
}

bool LoadDeferred(
    const std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
        &cached_toc,
    bool have_cached_toc, const std::string &markup_utf8, u32 text_len,
    const std::string &source_path,
    const StructuredCallbacks &structured_callbacks,
    const InlineTitleCallbacks &inline_title_callbacks,
    std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> *out,
    bool *structured_from_filepos, IStatusReporter *reporter) {
  const mobi_toc_resolver::PrepareCallbacks callbacks =
      BuildCallbacks(structured_callbacks, inline_title_callbacks);
  return mobi_toc_resolver::LoadDeferredStructuredToc(
      &cached_toc, have_cached_toc, markup_utf8, text_len, source_path,
      callbacks, out, structured_from_filepos, reporter);
}

} // namespace mobi_toc_prepare
