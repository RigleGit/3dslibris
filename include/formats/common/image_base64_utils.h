#pragma once

#include "formats/common/image_buffer.h"
#include "shared/base64_utils.h"

inline bool DecodeBase64Bytes(const char *input, size_t length,
                              image_memory::Bytes *out, size_t max_bytes) {
  if (!out)
    return false;
  out->clear();
  size_t size = 0;
  if (!base64_utils::CountDecodedBytes(input, length, max_bytes, &size) ||
      !out->resize(size, max_bytes))
    return false;
  if (!base64_utils::DecodeInto(input, length, out->mutable_data(), size)) {
    out->clear();
    return false;
  }
  return true;
}

inline bool DecodeBase64Bytes(const std::string &input,
                              image_memory::Bytes *out, size_t max_bytes) {
  return DecodeBase64Bytes(input.data(), input.size(), out, max_bytes);
}
