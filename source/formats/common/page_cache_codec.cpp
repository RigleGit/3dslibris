#include "formats/common/page_cache_codec.h"
#include <zlib.h>
#include <cstring>
namespace page_cache_codec {
bool Encode(const uint32_t *words,size_t count,std::vector<uint8_t> *out) {
  if(!out || count>kBlockWords || (count && !words)) return false;
  out->clear(); out->reserve(kBlockBytes);
  for(size_t i=0;i<count;++i) {
    uint32_t value=words[i];
    do {
      if(out->size()==kBlockBytes) { out->clear(); return false; }
      uint8_t byte=value&127; value>>=7;
      out->push_back(byte|(value ? 128 : 0));
    } while(value);
  }
  return true;
}
bool Decode(const uint8_t *bytes,size_t length,size_t count,std::vector<uint32_t> *out) {
  if(!out || count>kBlockWords || length>kBlockBytes || (length && !bytes)) return false;
  out->clear(); out->reserve(count); size_t cursor=0;
  for(size_t i=0;i<count;++i) {
    uint32_t value=0; unsigned shift=0;
    for(;;) {
      if(cursor==length) { out->clear(); return false; }
      const uint8_t byte=bytes[cursor++];
      if(shift==28 && (byte&240)) { out->clear(); return false; }
      value|=(uint32_t)(byte&127)<<shift;
      if(!(byte&128)) {
        if(shift && !(byte&127)) { out->clear(); return false; }
        break;
      }
      shift+=7;
      if(shift>28) { out->clear(); return false; }
    }
    out->push_back(value);
  }
  if(cursor!=length) { out->clear(); return false; }
  return true;
}
bool Pack(const std::vector<uint8_t> &encoded,std::vector<uint8_t> *out,bool *compressed) {
  if(!out || !compressed || encoded.size()>kBlockBytes) return false;
  *compressed=false;
  out->resize(compressBound(encoded.size()));
  uLongf length=out->size();
  int rc=compress2(out->data(),&length,encoded.data(),encoded.size(),1);
  if(rc!=Z_OK) { out->clear(); return false; }
  if(length>=encoded.size()) *out=encoded;
  else { out->resize(length); *compressed=true; }
  return true;
}
bool Unpack(const uint8_t *bytes,size_t length,size_t encoded_length,bool compressed,
            std::vector<uint8_t> *out) {
  if(!out || length>kBlockBytes || encoded_length>kBlockBytes || (length && !bytes)) return false;
  out->resize(encoded_length);
  if(!compressed) {
    if(length!=encoded_length) { out->clear(); return false; }
    if(length) memcpy(out->data(),bytes,length);
    return true;
  }
  uLongf decoded=encoded_length;
  if(uncompress(out->data(),&decoded,bytes,length)!=Z_OK || decoded!=encoded_length) {
    out->clear(); return false;
  }
  return true;
}
}
