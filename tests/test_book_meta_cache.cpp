#include "formats/common/book_meta_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

static int g_pass = 0;
static int g_fail = 0;

[[noreturn]] void Fail(const char *label, const char *reason) {
  fprintf(stderr, "FAIL %s: %s\n", label, reason);
  g_fail++;
  std::exit(1);
}

void ExpectTrue(const char *label, bool v) {
  if (!v) Fail(label, "expected true");
  g_pass++;
}

void ExpectFalse(const char *label, bool v) {
  if (v) Fail(label, "expected false");
  g_pass++;
}

void ExpectStrEq(const char *label, const std::string &actual, const char *expected) {
  if (actual != expected) {
    char buf[256];
    snprintf(buf, sizeof(buf), "expected '%s', got '%s'", expected, actual.c_str());
    Fail(label, buf);
  }
  g_pass++;
}

std::string MakeTempPath() {
  char path[] = "/tmp/3dslibris-meta-XXXXXX.bmc";
  int fd = mkstemps(path, 4);
  if (fd < 0) {
    perror("mkstemps");
    std::exit(1);
  }
  close(fd);
  return std::string(path);
}

void TestSaveLoadRoundtrip() {
  std::string path = MakeTempPath();

  book_meta_cache::MetaEntry entry;
  entry.title = "My Test Book";
  entry.author = "Test Author";
  entry.cover_image_path = "OEBPS/images/cover.jpg";

  ExpectTrue("save roundtrip: save succeeds", book_meta_cache::Save(path, entry));

  book_meta_cache::MetaEntry loaded;
  ExpectTrue("save roundtrip: load succeeds", book_meta_cache::Load(path, &loaded));
  ExpectStrEq("save roundtrip: title", loaded.title, "My Test Book");
  ExpectStrEq("save roundtrip: author", loaded.author, "Test Author");
  ExpectStrEq("save roundtrip: cover_image_path", loaded.cover_image_path,
              "OEBPS/images/cover.jpg");

  unlink(path.c_str());
}

void TestEmptyStrings() {
  std::string path = MakeTempPath();

  book_meta_cache::MetaEntry entry;
  entry.title = "";
  entry.author = "";
  entry.cover_image_path = "";

  ExpectTrue("empty strings: save", book_meta_cache::Save(path, entry));

  book_meta_cache::MetaEntry loaded;
  ExpectTrue("empty strings: load", book_meta_cache::Load(path, &loaded));
  ExpectStrEq("empty strings: title", loaded.title, "");
  ExpectStrEq("empty strings: author", loaded.author, "");
  ExpectStrEq("empty strings: cover", loaded.cover_image_path, "");

  unlink(path.c_str());
}

void TestUnicodeTitle() {
  std::string path = MakeTempPath();

  book_meta_cache::MetaEntry entry;
  entry.title = "三体";
  entry.author = "刘慈欣";
  entry.cover_image_path = "";

  ExpectTrue("unicode: save", book_meta_cache::Save(path, entry));

  book_meta_cache::MetaEntry loaded;
  ExpectTrue("unicode: load", book_meta_cache::Load(path, &loaded));
  ExpectStrEq("unicode: title", loaded.title, "三体");
  ExpectStrEq("unicode: author", loaded.author, "刘慈欣");

  unlink(path.c_str());
}

void TestLoadNonExistent() {
  book_meta_cache::MetaEntry loaded;
  bool ok = book_meta_cache::Load("/tmp/does_not_exist_3dslibris.bmc", &loaded);
  ExpectFalse("nonexistent: load fails", ok);
}

void TestLoadCorrupt() {
  std::string path = MakeTempPath();

  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) { unlink(path.c_str()); Fail("corrupt", "fopen failed"); }
  const char junk[] = "not a valid cache file at all!!!";
  fwrite(junk, 1, sizeof(junk) - 1, fp);
  fclose(fp);

  book_meta_cache::MetaEntry loaded;
  ExpectFalse("corrupt: load fails", book_meta_cache::Load(path, &loaded));

  unlink(path.c_str());
}

void TestLoadNullOut() {
  std::string path = MakeTempPath();
  book_meta_cache::MetaEntry entry;
  entry.title = "X";
  book_meta_cache::Save(path, entry);
  ExpectFalse("null out: load fails", book_meta_cache::Load(path, nullptr));
  unlink(path.c_str());
}

void TestSaveEmptyPath() {
  book_meta_cache::MetaEntry entry;
  entry.title = "X";
  ExpectFalse("empty path: save fails", book_meta_cache::Save("", entry));
}

void TestLoadEmptyPath() {
  book_meta_cache::MetaEntry loaded;
  ExpectFalse("empty path: load fails", book_meta_cache::Load("", &loaded));
}

} // namespace

int main() {
  TestSaveLoadRoundtrip();
  TestEmptyStrings();
  TestUnicodeTitle();
  TestLoadNonExistent();
  TestLoadCorrupt();
  TestLoadNullOut();
  TestSaveEmptyPath();
  TestLoadEmptyPath();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
