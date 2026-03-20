#include "formats/mobi/mobi_record_decode.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void Expect(bool condition, const char *message) {
  if (!condition)
    Fail(message);
}

void ExpectEq(const std::string &label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected) {
    Fail(label + ": expected [" + expected + "], got [" + actual + "]");
  }
}

void ExpectEqU32(const std::string &label, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    Fail(label + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(actual));
  }
}

void WriteBE16(std::string *buf, size_t offset, uint16_t value) {
  (*buf)[offset + 0] = (char)((value >> 8) & 0xFF);
  (*buf)[offset + 1] = (char)(value & 0xFF);
}

void WriteBE32(std::string *buf, size_t offset, uint32_t value) {
  (*buf)[offset + 0] = (char)((value >> 24) & 0xFF);
  (*buf)[offset + 1] = (char)((value >> 16) & 0xFF);
  (*buf)[offset + 2] = (char)((value >> 8) & 0xFF);
  (*buf)[offset + 3] = (char)(value & 0xFF);
}

void TestParseRecord0HeaderUsesWholeRecordOffsets() {
  std::string rec0(248, '\0');
  WriteBE16(&rec0, 0, 17480);
  WriteBE32(&rec0, 4, 123456);
  WriteBE16(&rec0, 8, 42);
  memcpy(&rec0[16], "MOBI", 4);
  WriteBE32(&rec0, 20, 232);
  WriteBE32(&rec0, 28, 65001);
  WriteBE32(&rec0, 84, 0x456);
  WriteBE32(&rec0, 88, 19);
  WriteBE32(&rec0, 108, 0x292);
  WriteBE32(&rec0, 112, 0x21);
  WriteBE32(&rec0, 116, 3);
  WriteBE32(&rec0, 240, 7);
  WriteBE32(&rec0, 244, 0x28F);

  mobi_record_decode::MobiRecord0Header header;
  Expect(mobi_record_decode::ParseRecord0Header((const uint8_t *)rec0.data(),
                                                rec0.size(), &header),
         "ParseRecord0Header should succeed");
  ExpectEqU32("compression", header.compression, 17480);
  ExpectEqU32("text_len", header.text_len, 123456);
  ExpectEqU32("text_rec_count", header.text_rec_count, 42);
  ExpectEqU32("encoding", header.encoding, 65001);
  ExpectEqU32("title_offset", header.title_offset, 0x456);
  ExpectEqU32("title_length", header.title_length, 19);
  ExpectEqU32("resource_start", header.resource_start, 0x292);
  ExpectEqU32("huffcdic_record_index", header.huffcdic_record_index, 0x21);
  ExpectEqU32("num_huffcdic_records", header.num_huffcdic_records, 3);
  ExpectEqU32("trailing_flags", header.trailing_flags, 7);
  ExpectEqU32("indx_index", header.indx_index, 0x28F);
}

void TestTrailingEntryHelpers() {
  ExpectEqU32("count bits set", (uint32_t)mobi_record_decode::CountBitsSet(14),
              3);
  ExpectEqU32("varlen from end",
              mobi_record_decode::GetVarLenFromEnd(
                  (const uint8_t *)"abcXY\x83", 6),
              3);

  ExpectEq("remove trailing varlen entry",
           mobi_record_decode::RemoveTrailingEntries(
               (const uint8_t *)"abcXY\x83", 6, 2),
           "abc");
  ExpectEq("remove trailing multibyte bytes",
           mobi_record_decode::RemoveTrailingEntries(
               (const uint8_t *)"abc12\x02", 6, 1),
           "abc");
}

void TestBuildMergedTextWithTrailingFlags() {
  std::string raw = "HEADER";
  raw += "abcXY";
  raw.push_back((char)0x83);
  std::vector<uint32_t> offsets;
  offsets.push_back(0);
  offsets.push_back(6);
  offsets.push_back((uint32_t)raw.size());

  mobi_record_decode::MobiRecord0Header header;
  header.compression = 1;
  header.text_len = 3;
  header.text_rec_count = 1;
  header.trailing_flags = 2;

  std::string merged;
  Expect(mobi_record_decode::BuildMergedText(raw, offsets, header, &merged),
         "BuildMergedText raw should succeed");
  ExpectEq("build merged text strips trailing entry", merged, "abc");
}

void TestBuildMergedTextWithPalmDoc() {
  std::string raw = "HEADER";
  raw.push_back((char)0xC1);
  raw.push_back('B');
  raw.push_back('C');
  std::vector<uint32_t> offsets;
  offsets.push_back(0);
  offsets.push_back(6);
  offsets.push_back((uint32_t)raw.size());

  mobi_record_decode::MobiRecord0Header header;
  header.compression = 2;
  header.text_len = 4;
  header.text_rec_count = 1;

  std::string merged;
  Expect(mobi_record_decode::BuildMergedText(raw, offsets, header, &merged),
         "BuildMergedText PalmDOC should succeed");
  ExpectEq("PalmDOC decode", merged, " ABC");
}

void TestBuildMergedTextWithHuffCdic() {
  const uint32_t off1 = 16;
  const uint32_t off2 = off1 + 256 * 4;

  std::string huff((size_t)off2 + 32 * 8, '\0');
  memcpy(&huff[0], "HUFF", 4);
  WriteBE32(&huff, 8, off1);
  WriteBE32(&huff, 12, off2);
  const uint32_t quick = 0x80u | 1u;
  for (size_t i = 0; i < 256; i++)
    WriteBE32(&huff, (size_t)off1 + i * 4, quick);

  std::string cdic(16, '\0');
  memcpy(&cdic[0], "CDIC", 4);
  WriteBE32(&cdic, 4, 16);
  WriteBE32(&cdic, 8, 1);
  WriteBE32(&cdic, 12, 1);
  cdic.push_back(0);
  cdic.push_back(2);
  cdic.push_back((char)0x80);
  cdic.push_back(1);
  cdic.push_back('A');

  std::string raw = "HEADER";
  raw.push_back('\0');
  std::vector<uint32_t> offsets;
  offsets.push_back(0);
  offsets.push_back(6);
  offsets.push_back(7);
  raw += huff;
  offsets.push_back((uint32_t)raw.size());
  raw += cdic;
  offsets.push_back((uint32_t)raw.size());

  mobi_record_decode::MobiRecord0Header header;
  header.compression = 17480;
  header.text_len = 8;
  header.text_rec_count = 1;
  header.huffcdic_record_index = 2;
  header.num_huffcdic_records = 2;

  std::string merged;
  Expect(mobi_record_decode::BuildMergedText(raw, offsets, header, &merged),
         "BuildMergedText HUFF/CDIC should succeed");
  ExpectEq("HUFF/CDIC decode", merged, "AAAAAAAA");
}

} // namespace

int main() {
  TestParseRecord0HeaderUsesWholeRecordOffsets();
  TestTrailingEntryHelpers();
  TestBuildMergedTextWithTrailingFlags();
  TestBuildMergedTextWithPalmDoc();
  TestBuildMergedTextWithHuffCdic();
  return 0;
}
