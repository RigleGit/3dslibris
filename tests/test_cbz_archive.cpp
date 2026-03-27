#include "formats/cbz/cbz_archive.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected '" + expected + "'");
}

void ExpectEqInt(const char *label, int actual, int expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected integer equality");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    Fail("usage: test_cbz_archive <archive.cbz>");

  std::vector<CbzPageEntry> entries;
  ExpectTrue("index archive", IndexCbzArchiveEntries(argv[1], &entries));
  ExpectEqInt("entry count", (int)entries.size(), 3);
  ExpectEq("entry 0", entries[0].normalized_path, "001-cover.jpg");
  ExpectEq("entry 1", entries[1].normalized_path, "002-page.png");
  ExpectEq("entry 2", entries[2].normalized_path, "010-last.jpeg");

  std::vector<unsigned char> bytes;
  ExpectTrue("read first entry",
             ReadCbzArchiveEntryBytes(argv[1], entries[0], &bytes, 1024 * 1024));
  ExpectEqInt("first entry non-empty", (int)!bytes.empty(), 1);
  ExpectTrue("read second entry",
             ReadCbzArchiveEntryBytes(argv[1], entries[1], &bytes, 1024 * 1024));
  ExpectEqInt("second entry non-empty", (int)!bytes.empty(), 1);
  ExpectTrue("read third entry",
             ReadCbzArchiveEntryBytes(argv[1], entries[2], &bytes, 1024 * 1024));
  ExpectEqInt("third entry non-empty", (int)!bytes.empty(), 1);
  return 0;
}
