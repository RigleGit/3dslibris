#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace mobi_record_decode {

constexpr uint32_t kNullIndex = 0xFFFFFFFFu;

struct MobiRecord0Header {
  uint16_t compression;
  uint32_t text_len;
  uint32_t text_rec_count;
  uint32_t encoding;
  uint32_t resource_start;
  uint32_t title_offset;
  uint32_t title_length;
  uint32_t huffcdic_record_index;
  uint32_t num_huffcdic_records;
  uint32_t trailing_flags;
  uint32_t indx_index;

  MobiRecord0Header();
};

bool ParseRecord0Header(const uint8_t *rec0, size_t rec0_len,
                        MobiRecord0Header *out);

size_t CountBitsSet(uint32_t x);
uint32_t GetVarLenFromEnd(const uint8_t *data, size_t len);
std::string RemoveTrailingEntries(const uint8_t *data, size_t len,
                                  uint32_t trailing_flags);

bool BuildMergedText(const std::string &raw, const std::vector<uint32_t> &offsets,
                     const MobiRecord0Header &header, std::string *merged);

} // namespace mobi_record_decode
