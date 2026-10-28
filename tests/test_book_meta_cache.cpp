#include "formats/common/book_meta_cache.h"
#include "shared/path_constants.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#undef ftell
extern "C" long ftell(FILE *);
static size_t index_tells = 0;
extern "C" long ObserveMetadataTell(FILE *fp) {
  ++index_tells;
  return ftell(fp);
}

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

void TestConsolidatedIndex() {
  using namespace book_meta_cache;
  std::vector<std::string> caches;
  for (int i = 0; i < 300; ++i) {
    MetaEntry entry;
    entry.title = "Título " + std::to_string(i);
    entry.author = "Autora " + std::to_string(300 - i);
    entry.series = "Serie"; entry.language = "es";
    entry.publisher = "Editorial"; entry.published = "2026";
    entry.subjects = "Libros"; entry.description = "Descripción";
    entry.cover_image_path = "OEBPS/cover.png";
    caches.push_back(BuildPath("books/" + std::to_string(i) + ".epub", 100 + i, 1234));
    ExpectTrue("index fixture saves real metadata", Save(caches.back(), entry));
  }
  // Existing installations migrate without parsing books or losing extended
  // fields. A second batch must work with every individual cache unavailable.
  unlink(paths::GetMetaIndexPath().c_str());
  {
    Index index;
    MetaEntry loaded;
    for (int i = 0; i < 300; ++i)
      ExpectTrue("index migrates individual caches", index.Load(caches[i], &loaded));
  }
  for (const auto &path : caches) ExpectTrue("remove individual cache", unlink(path.c_str()) == 0);
  index_tells = 0;
  {
    Index index;
    ExpectTrue("index streams records without per-book position queries", index_tells == 1);
    MetaEntry loaded;
    for (int i = 0; i < 300; ++i) {
      ExpectTrue("consolidated index serves a 300-book library", index.Load(caches[i], &loaded));
      ExpectStrEq("indexed title", loaded.title, ("Título " + std::to_string(i)).c_str());
      ExpectStrEq("indexed author", loaded.author, ("Autora " + std::to_string(300-i)).c_str());
      ExpectStrEq("indexed series", loaded.series, "Serie");
      ExpectStrEq("indexed language", loaded.language, "es");
      ExpectStrEq("indexed publisher", loaded.publisher, "Editorial");
      ExpectStrEq("indexed publication", loaded.published, "2026");
      ExpectStrEq("indexed subjects", loaded.subjects, "Libros");
      ExpectStrEq("indexed description", loaded.description, "Descripción");
      ExpectStrEq("indexed cover", loaded.cover_image_path, "OEBPS/cover.png");
    }
    ExpectFalse("different source size cannot reuse index", index.Load(BuildPath("books/0.epub", 101, 1234), &loaded));
    ExpectFalse("different source mtime cannot reuse index", index.Load(BuildPath("books/0.epub", 100, 1235), &loaded));
    ExpectFalse("different source path cannot reuse index", index.Load(BuildPath("other/0.epub", 100, 1234), &loaded));
  }
  MetaEntry replacement; replacement.title = "Replacement"; replacement.author = "New author";
  ExpectTrue("metadata job appends updated record", Save(caches[0], replacement));
  {
    Index index; MetaEntry loaded;
    ExpectTrue("updated journal record is available", index.Load(caches[0], &loaded));
    ExpectStrEq("newest metadata wins", loaded.title, "Replacement");
  }
  // An unwritable index file must not hide a successful individual-cache update.
  chmod(paths::GetMetaIndexPath().c_str(), 0400);
  replacement.title = "After read-only journal";
  ExpectTrue("individual update survives read-only journal", Save(caches[0], replacement));
  {
    Index index; MetaEntry loaded;
    ExpectTrue("read-only journal update is visible", index.Load(caches[0], &loaded));
    ExpectStrEq("no stale metadata after failed append open", loaded.title, "After read-only journal");
  }
  for (bool truncate : {false, true}) {
    ExpectTrue("save intact prefix record", Save(caches[1], replacement));
    ExpectTrue("save final record to corrupt", Save(caches[2], replacement));
    unlink(caches[1].c_str()); unlink(caches[2].c_str());
    FILE *fp = fopen(paths::GetMetaIndexPath().c_str(), "r+b");
    ExpectTrue("open journal for corruption", fp != nullptr);
    if (truncate) {
      fseek(fp, 0, SEEK_END);
      ExpectTrue("truncate final checksum", ftruncate(fileno(fp), ftell(fp) - 3) == 0);
    } else {
      fseek(fp, -1, SEEK_END); const int last = fgetc(fp);
      fseek(fp, -1, SEEK_END); fputc(last ^ 0x80, fp);
    }
    fclose(fp);
    {
      Index index; MetaEntry loaded;
      ExpectTrue("corrupt journal falls back to individual cache", index.Load(caches[0], &loaded));
      ExpectStrEq("fallback retains current metadata", loaded.title, "After read-only journal");
      ExpectFalse("corrupt journal cannot serve an unvalidated prefix", index.Load(caches[1], &loaded));
    }
  }
  // The existing clear-cache action deletes every file inside meta/, including
  // the journal and its temporary replacement. Neither is outside that owner.
  ExpectTrue("journal belongs to metadata cache directory",
      paths::GetMetaIndexPath().find(paths::GetMetaCacheDir() + "/") == 0);
  unlink(paths::GetMetaIndexPath().c_str()); unlink(caches[0].c_str());
  Index empty; MetaEntry loaded;
  ExpectFalse("cleared metadata is not resurrected", empty.Load(caches[0], &loaded));
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
  TestConsolidatedIndex();

  fprintf(stderr, "Results: %d/%d passed, %d failed\n", g_pass, g_pass + g_fail,
          g_fail);
  return g_fail > 0 ? 1 : 0;
}
