#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace utf8_utils {

bool IsValidUtf8(const std::string &s);
bool IsValidUtf8(const char *s);

std::string DecodeCp1252ToUtf8(const std::string &in);
bool TryRepairMojibakeUtf8(const std::string &in, std::string *out);
bool TryRepairFullwidthByteMojibake(const std::string &in, std::string *out);
std::string ComposeLatinCombiningMarks(const std::string &in);

bool Utf16NameToUtf8(const uint16_t *name, std::string *out,
                     size_t max_units = 0x106);
std::string NormalizeFsFilenameForIo(const std::string &raw_name);

size_t CountUtf8InvalidLeadBytes(const std::string &bytes);
std::string DecodeMostlyUtf8WithCp1252Fallback(const std::string &in,
                                               size_t *invalid_out);

} // namespace utf8_utils
