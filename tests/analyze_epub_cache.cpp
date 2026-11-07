#include "formats/common/page_cache_codec.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>
#include <zlib.h>
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
struct Totals {
  size_t raw = 0, varint = 0, zraw = 0, zvarint = 0;
  double encode = 0, decode = 0, compress_raw = 0, inflate_raw = 0,
         compress_varint = 0, inflate_varint = 0;
};
static bool Flush(std::vector<uint32_t> &words, Totals &t) {
  if (words.empty())
    return true;
  std::vector<uint8_t> encoded, packed, unpacked,
      raw_packed(compressBound(words.size() * 4));
  std::vector<uint32_t> decoded;
  auto begin = Clock::now();
  if (!page_cache_codec::Encode(words.data(), words.size(), &encoded))
    return false;
  t.encode += Ms(begin);
  begin = Clock::now();
  if (!page_cache_codec::Decode(encoded.data(), encoded.size(), words.size(),
                                &decoded) ||
      decoded != words)
    return false;
  t.decode += Ms(begin);
  bool compressed = false;
  begin = Clock::now();
  if (!page_cache_codec::Pack(encoded, &packed, &compressed))
    return false;
  t.compress_varint += Ms(begin);
  begin = Clock::now();
  if (!page_cache_codec::Unpack(packed.data(), packed.size(), encoded.size(),
                                compressed, &unpacked) ||
      unpacked != encoded)
    return false;
  t.inflate_varint += Ms(begin);
  uLongf size = raw_packed.size();
  begin = Clock::now();
  if (compress2(raw_packed.data(), &size, (const Bytef *)words.data(),
                words.size() * 4, 1) != Z_OK)
    return false;
  t.compress_raw += Ms(begin);
  std::vector<uint32_t> raw_decoded(words.size());
  uLongf decoded_size = words.size() * 4;
  begin = Clock::now();
  if (uncompress((Bytef *)raw_decoded.data(), &decoded_size, raw_packed.data(),
                 size) != Z_OK ||
      raw_decoded != words)
    return false;
  t.inflate_raw += Ms(begin);
  t.raw += words.size() * 4;
  t.varint += encoded.size();
  t.zraw += size;
  t.zvarint += packed.size();
  words.clear();
  return true;
}
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s /path/to/cache-v22.epc\n", argv[0]);
    return 2;
  }
  FILE *fp = fopen(argv[1], "rb");
  if (!fp) {
    perror("open cache");
    return 1;
  }
  setvbuf(fp, nullptr, _IOFBF, 65536);
  unsigned char header[32];
  if (fread(header, 1, 32, fp) != 32) {
    fclose(fp);
    return 1;
  }
  uint32_t magic = 0, pages = 0;
  uint16_t version = 0, title = 0;
  memcpy(&magic, header, 4);
  memcpy(&version, header + 4, 2);
  memcpy(&title, header + 6, 2);
  memcpy(&pages, header + 8, 4);
  if (magic != 0x45504347U || version != 22 || !pages || pages > 50000 ||
      title > 1000 || fseek(fp, title, SEEK_CUR) != 0) {
    fprintf(stderr, "expected a valid v22 header\n");
    fclose(fp);
    return 1;
  }
  Totals totals;
  std::vector<uint32_t> batch;
  batch.reserve(8192);
  bool ok = true;
  for (unsigned section = 0; ok && section < 2; ++section)
    for (unsigned page = 0; ok && page < pages; ++page) {
      uint16_t count = 0;
      if (fread(&count, 1, 2, fp) != 2 || count > (section ? 4097 : 4096)) {
        ok = false;
        break;
      }
      if (batch.size() + count > 8192)
        ok = Flush(batch, totals);
      const size_t before = batch.size();
      batch.resize(before + count);
      if (count && fread(batch.data() + before, 4, count, fp) != count)
        ok = false;
    }
  if (ok)
    ok = Flush(batch, totals);
  fclose(fp);
  if (!ok) {
    fprintf(stderr, "invalid payload or failed roundtrip\n");
    return 1;
  }
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  size_t rss = usage.ru_maxrss;
#ifndef __APPLE__
  rss *= 1024;
#endif
  printf("payload only; metadata and page record prefixes excluded; host CPU, "
         "not ARM timings\n");
  printf("u32 bytes=%zu\nvarint bytes=%zu encode_ms=%.3f "
         "decode_ms=%.3f\nu32+zlib1 bytes=%zu encode_ms=%.3f "
         "decode_ms=%.3f\nvarint+zlib1 bytes=%zu encode_ms=%.3f "
         "decode_ms=%.3f\npeak_rss_process_bytes=%zu\n",
         totals.raw, totals.varint, totals.encode, totals.decode, totals.zraw,
         totals.compress_raw, totals.inflate_raw, totals.zvarint,
         totals.encode + totals.compress_varint,
         totals.inflate_varint + totals.decode, rss);
}
