#include "shared/mobi_cover_utils.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

[[noreturn]] void Fail(const char *msg) {
  std::fprintf(stderr, "%s\n", msg);
  std::exit(1);
}

void WriteBE16(std::vector<uint8_t> *buf, size_t off, uint16_t value) {
  (*buf)[off + 0] = (uint8_t)((value >> 8) & 0xFF);
  (*buf)[off + 1] = (uint8_t)(value & 0xFF);
}

void WriteBE32(std::vector<uint8_t> *buf, size_t off, uint32_t value) {
  (*buf)[off + 0] = (uint8_t)((value >> 24) & 0xFF);
  (*buf)[off + 1] = (uint8_t)((value >> 16) & 0xFF);
  (*buf)[off + 2] = (uint8_t)((value >> 8) & 0xFF);
  (*buf)[off + 3] = (uint8_t)(value & 0xFF);
}

void TestOffsetTableSizeUsesRecordCount() {
  std::vector<uint8_t> header(94, 0);
  WriteBE16(&header, 76, 2);
  const size_t size =
      mobi_cover_utils::PdbOffsetTableSizeFromHeader(header.data(), header.size());
  if (size != 94)
    Fail("pdb offset table size should include all record entries");
}

void TestParseOffsetsUsesFileSizeForSentinel() {
  std::vector<uint8_t> header(94, 0);
  WriteBE16(&header, 76, 2);
  WriteBE32(&header, 78, 120);
  WriteBE32(&header, 86, 640);

  std::vector<uint32_t> offsets;
  if (!mobi_cover_utils::ParsePdbRecordOffsets(header.data(), header.size(),
                                               1000, &offsets)) {
    Fail("offset table should parse with external file size");
  }
  if (offsets.size() != 3 || offsets[0] != 120 || offsets[1] != 640 ||
      offsets[2] != 1000) {
    Fail("parsed offsets should preserve records and final file sentinel");
  }
}

void TestParseOffsetsRejectsOutOfFileBounds() {
  std::vector<uint8_t> header(94, 0);
  WriteBE16(&header, 76, 2);
  WriteBE32(&header, 78, 120);
  WriteBE32(&header, 86, 1200);

  std::vector<uint32_t> offsets;
  if (mobi_cover_utils::ParsePdbRecordOffsets(header.data(), header.size(),
                                              1000, &offsets)) {
    Fail("offset parser should reject records beyond file bounds");
  }
}

} // namespace

int main() {
  TestOffsetTableSizeUsesRecordCount();
  TestParseOffsetsUsesFileSizeForSentinel();
  TestParseOffsetsRejectsOutOfFileBounds();
  return 0;
}
