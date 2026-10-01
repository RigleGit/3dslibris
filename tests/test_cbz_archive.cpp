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

int main() {
  const char *archive = std::getenv("TEST_CBZ_ARCHIVE_PATH");
  ExpectTrue("fixture path", archive != NULL);
  const char *expected[] = {
      "001-cover.jpg", "002-page.PNG",
      "Chapter 2/2.png", "Chapter 2/02.png", "Chapter 2/10.png",
      "Chapter 02/1.png", "Chapter 5/1.png", "Chapter 10/1.png",
      "Volume 1/Chapter 2/1.png", "Volume 1/Chapter 10/1.png",
      "Volume 2/1.png", "folder/../010-last.JPEG", "nested/003-middle.jpg",
      "number9.png", "number10.png", "number999999999999999999999.png",
      "number1000000000000000000000.png", "zero0.png", "zero00.png"};

  std::vector<CbzPageEntry> entries;
  ExpectTrue("index archive", IndexCbzArchiveEntries(archive, &entries));
  ExpectEqInt("entry count", (int)entries.size(),
              (int)(sizeof(expected) / sizeof(expected[0])));
  CbzArchiveReader reader;
  for (size_t i = 0; i < entries.size(); i++) {
    ExpectEq("natural page order", entries[i].normalized_path, expected[i]);
    std::vector<unsigned char> bytes;
    ExpectTrue("read sorted entry by ZIP offset",
               ReadCbzArchiveEntryBytes(archive, entries[i], &bytes, 1024));
    ExpectEq("sorted entry payload", std::string(bytes.begin(), bytes.end()),
             expected[i]);
    ExpectTrue("read with retained archive",
               reader.Read(archive, entries[i], &bytes, 1024));
    ExpectEq("retained archive payload", std::string(bytes.begin(), bytes.end()),
             expected[i]);
    CbzPageEntry by_name = entries[i];
    by_name.offset = 0;
    ExpectTrue("read sorted entry by name",
               ReadCbzArchiveEntryBytes(archive, by_name, &bytes, 1024));
    ExpectEq("name lookup payload", std::string(bytes.begin(), bytes.end()),
             expected[i]);
  }

  std::vector<unsigned char> bytes;
  reader.Close();
  reader.Close();
  ExpectTrue("reopen after explicit close", reader.Read(archive, entries[0], &bytes, 1024));
  ExpectTrue("accept exact byte limit", reader.Read(archive, entries[0], &bytes, std::string(expected[0]).size()));
  ExpectTrue("reject oversized entry", !reader.Read(archive, entries[0], &bytes, std::string(expected[0]).size() - 1));
  ExpectTrue("oversized read clears previous bytes", bytes.empty());
  ExpectTrue("recover after read error", reader.Read(archive, entries[0], &bytes, 1024));
  ExpectTrue("reject missing archive", !reader.Read(std::string(archive) + ".missing", entries[0], &bytes, 1024));
  ExpectTrue("recover after switching path", reader.Read(archive, entries[0], &bytes, 1024));
  ExpectEq("reopened payload", std::string(bytes.begin(), bytes.end()), expected[0]);

  CbzPageEntry missing = entries[0];
  missing.path = "absent.png";
  missing.offset = 0;
  ExpectTrue("missing member rejected", !reader.Read(archive, missing, &bytes, 1024));
  ExpectTrue("missing member clears previous bytes", bytes.empty());
  ExpectTrue("missing member supplies error", GetLastCbzArchiveError()[0] != '\0');
  ExpectTrue("valid member after locate failure", reader.Read(archive, entries[0], &bytes, 1024));
  ExpectTrue("successful read clears stale error", GetLastCbzArchiveError()[0] == '\0');

  const char *invalid_archive = std::getenv("TEST_CBZ_INVALID_PATH");
  ExpectTrue("invalid archive fixture supplied", invalid_archive != NULL);
  ExpectTrue("reject actual non-ZIP file", !IndexCbzArchiveEntries(invalid_archive, &entries));
  ExpectTrue("failed archive index clears stale entries", entries.empty());
  ExpectTrue("reindex valid archive after error", IndexCbzArchiveEntries(archive, &entries));

  std::vector<CbzComicInfoBookmark> bookmarks;
  ExpectTrue("read ComicInfo bookmarks", ReadComicInfoBookmarks(archive, &bookmarks));
  ExpectEqInt("bookmark count", (int)bookmarks.size(), 2);
  ExpectEq("cover bookmark", entries[bookmarks[0].image_index].normalized_path,
           "001-cover.jpg");
  ExpectEq("chapter bookmark", entries[bookmarks[1].image_index].normalized_path,
           "Chapter 5/1.png");
  ExpectEq("chapter title", bookmarks[1].title, "Chapter five");
  ExpectTrue("bad archive has no bookmarks", !ReadComicInfoBookmarks(invalid_archive, &bookmarks));
  ExpectTrue("bad archive clears previous bookmarks", bookmarks.empty());
  puts("PASS: CBZ natural order, ZIP reads and ComicInfo bookmarks");
  return 0;
}
