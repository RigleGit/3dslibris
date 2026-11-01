#include "formats/common/page_cache_codec.h"
#include "formats/common/cache_memory_utils.h"
#include <zlib.h>
#include <cstring>
#include <new>
namespace page_cache_codec {
bool Append(const uint32_t *words, size_t count, std::vector<uint8_t> *out) {
  if (!out || count > kBlockWords || (count && !words))
    return false;
  const size_t before = out->size();
  if (before > kBlockBytes)
    return false;
  out->reserve(kBlockBytes);
  for (size_t i = 0; i < count; ++i) {
    uint32_t value = words[i];
    do {
      if (out->size() == kBlockBytes) {
        out->resize(before);
        return false;
      }
      uint8_t byte = value & 127;
      value >>= 7;
      out->push_back(byte | (value ? 128 : 0));
    } while (value);
  }
  return true;
}
bool Encode(const uint32_t *words, size_t count, std::vector<uint8_t> *out) {
  if (!out)
    return false;
  out->clear();
  return Append(words, count, out);
}
bool Decode(const uint8_t *bytes, size_t length, size_t count,
            std::vector<uint32_t> *out) {
  if (!out || count > kBlockWords || length > kBlockBytes || (length && !bytes))
    return false;
  out->clear();
  out->reserve(count);
  size_t cursor = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t value = 0;
    unsigned shift = 0;
    for (;;) {
      if (cursor == length) {
        out->clear();
        return false;
      }
      const uint8_t byte = bytes[cursor++];
      if (shift == 28 && (byte & 240)) {
        out->clear();
        return false;
      }
      value |= (uint32_t)(byte & 127) << shift;
      if (!(byte & 128)) {
        if (shift && !(byte & 127)) {
          out->clear();
          return false;
        }
        break;
      }
      shift += 7;
      if (shift > 28) {
        out->clear();
        return false;
      }
    }
    out->push_back(value);
  }
  if (cursor != length) {
    out->clear();
    return false;
  }
  return true;
}
struct Compressor::State {
  z_stream stream;
  bool initialized;
  State() : initialized(false) {
    memset(&stream, 0, sizeof(stream));
  }
  ~State() {
    if (initialized)
      deflateEnd(&stream);
  }
};
Compressor::Compressor() : state_(new (std::nothrow) State) {}
Compressor::~Compressor() {
  delete state_;
}
bool Compressor::Pack(const std::vector<uint8_t> &encoded,
                      std::vector<uint8_t> *out, bool *compressed) {
  if (!state_ || !out || !compressed || encoded.size() > kBlockBytes)
    return false;
  State &s = *state_;
  if (!s.initialized) {
    if (deflateInit2(&s.stream, 1, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK)
      return false;
    s.initialized = true;
  } else if (deflateReset(&s.stream) != Z_OK)
    return false;
  const size_t capacity = compressBound(kBlockBytes);
  if (out->capacity() < capacity) {
    if (!cache_memory::HasHeadroom(capacity, 0))
      return false;
    out->reserve(capacity);
  }
  out->resize(compressBound(encoded.size()));
  s.stream.next_in = const_cast<Bytef *>(encoded.data());
  s.stream.avail_in = encoded.size();
  s.stream.next_out = out->data();
  s.stream.avail_out = out->size();
  if (deflate(&s.stream, Z_FINISH) != Z_STREAM_END) {
    out->clear();
    return false;
  }
  *compressed = s.stream.total_out < encoded.size();
  if (*compressed)
    out->resize(s.stream.total_out);
  else
    *out = encoded;
  return true;
}
bool Pack(const std::vector<uint8_t> &encoded, std::vector<uint8_t> *out,
          bool *compressed) {
  Compressor compressor;
  return compressor.Pack(encoded, out, compressed);
}
bool Unpack(const uint8_t *bytes, size_t length, size_t encoded_length,
            bool compressed, std::vector<uint8_t> *out, bool *out_of_memory) {
  if (out_of_memory)
    *out_of_memory = false;
  if (!out || length > kBlockBytes || encoded_length > kBlockBytes ||
      (length && !bytes))
    return false;
  out->resize(encoded_length);
  if (!compressed) {
    if (length != encoded_length) {
      out->clear();
      return false;
    }
    if (length)
      memcpy(out->data(), bytes, length);
    return true;
  }
  uLongf decoded = encoded_length;
  const int result = uncompress(out->data(), &decoded, bytes, length);
  if (result != Z_OK || decoded != encoded_length) {
    if (out_of_memory)
      *out_of_memory = result == Z_MEM_ERROR;
    out->clear();
    return false;
  }
  return true;
}
} // namespace page_cache_codec
