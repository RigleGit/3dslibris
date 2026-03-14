/*
    3dslibris - mobi_text_cleanup.h

    Summary:
    - Conservative text cleanup helpers for malformed MOBI prose.
    - Used only for per-book repair of hard-wrapped line breaks.
*/

#pragma once

#include <string>

namespace mobi_text_cleanup {

std::string RepairCommonMojibake(const std::string &text);
std::string FixBrokenParagraphWraps(const std::string &text);

} // namespace mobi_text_cleanup
