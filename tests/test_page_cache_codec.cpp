#include "formats/common/page_cache_codec.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
int main() {
  const uint32_t words[]={0,127,128,16383,16384,0xffffffffU};
  const uint8_t wire[]={0,127,128,1,255,127,128,128,1,255,255,255,255,15};
  std::vector<uint8_t> encoded;
  std::vector<uint32_t> decoded;
  assert(page_cache_codec::Encode(words,6,&encoded));
  assert(encoded==std::vector<uint8_t>(wire,wire+sizeof(wire)));
  assert(page_cache_codec::Decode(wire,sizeof(wire),6,&decoded));
  assert(decoded==std::vector<uint32_t>(words,words+6));
  const uint8_t overflow[]={255,255,255,255,31},overlong[]={128,0},shortword[]={128};
  assert(!page_cache_codec::Decode(overflow,sizeof(overflow),1,&decoded));
  assert(!page_cache_codec::Decode(overlong,sizeof(overlong),1,&decoded));
  assert(!page_cache_codec::Decode(shortword,sizeof(shortword),1,&decoded));
  assert(!page_cache_codec::Decode(wire,sizeof(wire),5,&decoded));
  std::vector<uint32_t> large(16384,'a');
  assert(page_cache_codec::Encode(large.data(),large.size(),&encoded));
  std::vector<uint8_t> packed,unpacked; bool compressed=false;
  assert(page_cache_codec::Pack(encoded,&packed,&compressed) && compressed);
  assert(packed.size()<encoded.size());
  assert(page_cache_codec::Unpack(packed.data(),packed.size(),encoded.size(),compressed,&unpacked));
  assert(unpacked==encoded);
  assert(!page_cache_codec::Unpack(packed.data(),packed.size(),65537,true,&unpacked));
  packed[packed.size()/2]^=0x40;
  assert(!page_cache_codec::Unpack(packed.data(),packed.size(),encoded.size(),true,&unpacked));
  assert(!page_cache_codec::Encode(large.data(),large.size()+1,&encoded));
  puts("page cache codec: passed");
}
