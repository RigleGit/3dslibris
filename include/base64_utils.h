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

#include "3ds.h" // for u8

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

inline bool DecodeBase64Bytes(const std::string &in, std::vector<u8> *out,
                              size_t max_bytes) {
  if (!out)
    return false;
  out->clear();
  if (in.empty())
    return false;

  out->reserve((in.size() * 3) / 4);
  int accum = 0;
  int bits = -8;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '=')
      break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    int v = Base64Value(c);
    if (v < 0)
      return false;
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 0) {
      out->push_back((u8)((accum >> bits) & 0xFF));
      bits -= 8;
      if (out->size() > max_bytes)
        return false;
    }
  }
  return !out->empty();
}

#endif // BASE64_UTILS_H
