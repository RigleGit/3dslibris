#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mobi_cover_utils {

inline uint16_t ReadBE16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

inline uint32_t ReadBE32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

inline bool ParsePdbRecordOffsets(const uint8_t *data, size_t size,
                                  uint32_t file_size,
                                  std::vector<uint32_t> *offsets) {
  if (!data || !offsets || size < 78)
    return false;

  const uint16_t rec_count = ReadBE16(data + 76);
  if (rec_count == 0 || size < 78 + (size_t)rec_count * 8)
    return false;

  offsets->assign((size_t)rec_count + 1, 0);
  for (uint16_t i = 0; i < rec_count; i++) {
    const uint32_t off = ReadBE32(data + 78 + (size_t)i * 8);
    if (off >= file_size)
      return false;
    if (i > 0 && off < (*offsets)[(size_t)i - 1])
      return false;
    (*offsets)[(size_t)i] = off;
  }
  (*offsets)[(size_t)rec_count] = file_size;
  return true;
}

inline bool ParsePdbRecordOffsets(const uint8_t *data, size_t size,
                                  std::vector<uint32_t> *offsets) {
  return ParsePdbRecordOffsets(data, size, (uint32_t)size, offsets);
}

inline size_t PdbOffsetTableSizeFromHeader(const uint8_t *data, size_t size) {
  if (!data || size < 78)
    return 0;
  const uint16_t rec_count = ReadBE16(data + 76);
  if (rec_count == 0)
    return 0;
  return 78 + (size_t)rec_count * 8;
}

} // namespace mobi_cover_utils
