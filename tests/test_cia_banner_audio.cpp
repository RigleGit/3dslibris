#include "test_assert.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

uint16_t ReadU16LE(const std::vector<unsigned char> &buf, size_t off) {
  return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

uint32_t ReadU32LE(const std::vector<unsigned char> &buf, size_t off) {
  return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
         ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

std::vector<unsigned char> ReadFile(const char *path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::vector<unsigned char>();
  in.seekg(0, std::ios::end);
  std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> buf((size_t)size);
  if (!buf.empty())
    in.read((char *)&buf[0], size);
  return buf;
}

void TestBannerAudioIsAudible() {
  const std::vector<unsigned char> wav = ReadFile("assets/cia/BannerAudio.wav");
  test::ExpectTrue("banner wav exists", !wav.empty());
  test::ExpectTrue("banner wav has RIFF header", wav.size() >= 44);
  test::ExpectTrue("RIFF", std::string((const char *)&wav[0], 4) == "RIFF");
  test::ExpectTrue("WAVE", std::string((const char *)&wav[8], 4) == "WAVE");

  size_t pos = 12;
  bool have_fmt = false;
  bool have_data = false;
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  size_t data_start = 0;
  size_t data_size = 0;

  while (pos + 8 <= wav.size()) {
    const std::string chunk((const char *)&wav[pos], 4);
    const uint32_t chunk_size = ReadU32LE(wav, pos + 4);
    const size_t chunk_data = pos + 8;
    if (chunk_data + chunk_size > wav.size())
      break;

    if (chunk == "fmt " && chunk_size >= 16) {
      audio_format = ReadU16LE(wav, chunk_data);
      channels = ReadU16LE(wav, chunk_data + 2);
      sample_rate = ReadU32LE(wav, chunk_data + 4);
      bits_per_sample = ReadU16LE(wav, chunk_data + 14);
      have_fmt = true;
    } else if (chunk == "data") {
      data_start = chunk_data;
      data_size = chunk_size;
      have_data = true;
    }

    pos = chunk_data + chunk_size + (chunk_size & 1);
  }

  test::ExpectTrue("fmt chunk", have_fmt);
  test::ExpectTrue("data chunk", have_data);
  test::ExpectEq("PCM format", (int)audio_format, 1);
  test::ExpectEq("mono", (int)channels, 1);
  test::ExpectEq("16 kHz", (int)sample_rate, 16000);
  test::ExpectEq("16 bit", (int)bits_per_sample, 16);
  test::ExpectTrue("has samples", data_size >= 16000);

  int max_abs_sample = 0;
  const size_t end = std::min(wav.size(), data_start + data_size);
  for (size_t i = data_start; i + 1 < end; i += 2) {
    int16_t sample = (int16_t)ReadU16LE(wav, i);
    int abs_sample = sample < 0 ? -sample : sample;
    if (abs_sample > max_abs_sample)
      max_abs_sample = abs_sample;
  }

  test::ExpectTrue("banner wav is not silent", max_abs_sample > 512);
}

} // namespace

int main() {
  TestBannerAudioIsAudible();
  return 0;
}
