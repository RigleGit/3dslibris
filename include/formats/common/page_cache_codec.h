#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace page_cache_codec {
static const size_t kBlockBytes=65536;
static const size_t kBlockWords=kBlockBytes/sizeof(uint32_t);
bool Encode(const uint32_t *words,size_t count,std::vector<uint8_t> *out);
bool Decode(const uint8_t *bytes,size_t length,size_t count,std::vector<uint32_t> *out);
bool Pack(const std::vector<uint8_t> &encoded,std::vector<uint8_t> *out,bool *compressed);
bool Unpack(const uint8_t *bytes,size_t length,size_t encoded_length,bool compressed,
            std::vector<uint8_t> *out);
}
