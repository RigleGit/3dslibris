/*
    3dslibris - mobi_toc_prepare.h

    Wrapper helpers for MOBI structured TOC prepare/deferred load.
*/

#pragma once

#include <3ds/types.h>

#include <string>
#include <vector>

#include "formats/mobi/mobi_structured_toc_parser.h"

class IStatusReporter;

namespace mobi_toc_prepare {

struct StructuredCallbacks {
  std::string (*decode_bytes_to_utf8)(const std::string &in, u32 encoding);
  std::string (*normalize_title)(const std::string &in);
  bool (*reject_title)(const std::string &in);
};

struct InlineTitleCallbacks {
  bool (*looks_like_structured_title)(const std::string &title);
  std::string (*fold_latin_for_match)(const std::string &text);
  size_t (*count_ascii_words)(const std::string &text);
  bool (*is_mostly_digits_or_punctuation)(const std::string &text);
};

bool Prepare(const std::string &raw, const std::vector<u32> &offsets,
             u32 ncx_index, u32 encoding, const std::string *markup_utf8,
             u32 text_len, const StructuredCallbacks &structured_callbacks,
             const InlineTitleCallbacks &inline_title_callbacks,
             std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
                 *out,
             bool *structured_from_filepos, IStatusReporter *reporter);

bool LoadDeferred(
    const std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry>
        &cached_toc,
    bool have_cached_toc, const std::string &markup_utf8, u32 text_len,
    const std::string &source_path,
    const StructuredCallbacks &structured_callbacks,
    const InlineTitleCallbacks &inline_title_callbacks,
    std::vector<mobi_structured_toc_parser::MobiStructuredTocEntry> *out,
    bool *structured_from_filepos, IStatusReporter *reporter);

} // namespace mobi_toc_prepare
