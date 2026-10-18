/*
    3dslibris - base64_utils.h
    Shared Base64 decoding extracted from fb2.cpp and book_inline_image.cpp.
    Created by Rigle to reduce code duplication.
*/

#ifndef BASE64_UTILS_H
#define BASE64_UTILS_H

#include <stdint.h>
#include <string>
#include <vector>

#if __has_include(<3ds.h>)
#include <3ds.h> // for u8 on 3DS builds
#else
typedef unsigned char u8;
#endif

inline int Base64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+' || c == '-')
    return 62;
  if (c == '/' || c == '_')
    return 63;
  return -1;
}

namespace base64_utils {

inline bool CountDecodedBytes(const char *input, size_t length,
                              size_t max_bytes, size_t *decoded_size) {
  if (!decoded_size)
    return false;
  *decoded_size = 0;
  if (!input || !length)
    return false;
  size_t count = 0;
  int bits = -8;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)input[i];
    if (c == '=')
      break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    if (Base64Value(c) < 0)
      return false;
    bits += 6;
    if (bits >= 0) {
      if (count >= max_bytes)
        return false;
      count++;
      bits -= 8;
    }
  }
  *decoded_size = count;
  return count != 0;
}

inline bool DecodeInto(const char *input, size_t length, u8 *out,
                        size_t size) {
  if (!input || !out || !size)
    return false;
  uint32_t accum = 0;
  int bits = -8;
  size_t written = 0;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)input[i];
    if (c == '=')
      break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    int v = Base64Value(c);
    if (v < 0)
      return false;
    accum = (accum << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 0) {
      if (written >= size)
        return false;
      out[written++] = (u8)((accum >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return written == size;
}

} // namespace base64_utils

inline bool DecodeBase64Bytes(const std::string &in, std::vector<u8> *out,
                              size_t max_bytes) {
  if (!out)
    return false;
  out->clear();
  size_t size = 0;
  if (!base64_utils::CountDecodedBytes(in.data(), in.size(), max_bytes, &size))
    return false;
  out->resize(size);
  if (!base64_utils::DecodeInto(in.data(), in.size(), out->data(), size)) {
    out->clear();
    return false;
  }
  return true;
}

#endif // BASE64_UTILS_H
