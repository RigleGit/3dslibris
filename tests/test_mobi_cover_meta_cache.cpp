#include "mobi_cover_meta_cache.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, unsigned actual, unsigned expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

} // namespace

int main() {
  using mobi_cover_meta_cache::BuildPath;
  using mobi_cover_meta_cache::CoverMeta;
  using mobi_cover_meta_cache::Load;
  using mobi_cover_meta_cache::ResultKind;
  using mobi_cover_meta_cache::Save;

  std::string path = BuildPath("/tmp/example.mobi", 1234, 5678);
  ExpectFalse("cache path not empty", path.empty());

  char tmp_name[] = "/tmp/3dslibris-mobi-cover-meta-XXXXXX";
  int fd = mkstemp(tmp_name);
  if (fd < 0)
    Fail("mkstemp failed");
  fclose(fdopen(fd, "r"));

  CoverMeta saved{};
  saved.kind = ResultKind::kCandidate;
  saved.record_index = 42;
  saved.record_start = 1000;
  saved.record_end = 1600;
  saved.image_offset = 32;
  saved.width = 600;
  saved.height = 800;

  ExpectTrue("save candidate meta", Save(tmp_name, saved));

  CoverMeta loaded{};
  ExpectTrue("load candidate meta", Load(tmp_name, &loaded));
  ExpectEq("loaded kind", (unsigned)loaded.kind, (unsigned)ResultKind::kCandidate);
  ExpectEq("loaded record index", loaded.record_index, 42);
  ExpectEq("loaded record start", loaded.record_start, 1000);
  ExpectEq("loaded record end", loaded.record_end, 1600);
  ExpectEq("loaded image offset", loaded.image_offset, 32);
  ExpectEq("loaded width", loaded.width, 600);
  ExpectEq("loaded height", loaded.height, 800);

  saved = CoverMeta{};
  saved.kind = ResultKind::kNoCover;
  ExpectTrue("save no-cover meta", Save(tmp_name, saved));
  loaded = CoverMeta{};
  ExpectTrue("load no-cover meta", Load(tmp_name, &loaded));
  ExpectEq("loaded no-cover kind", (unsigned)loaded.kind,
           (unsigned)ResultKind::kNoCover);

  std::remove(tmp_name);
  return 0;
}
