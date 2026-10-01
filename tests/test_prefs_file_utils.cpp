#include "settings/prefs_file_utils.h"
#include <cassert>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void Put(const std::string &path, const char *value) {
  FILE *f = fopen(path.c_str(), "wb"); assert(f);
  assert(fputs(value, f) >= 0); assert(fclose(f) == 0);
}
static std::string Read(FILE *f) {
  assert(f); char b[256] = {}; const size_t n = fread(b, 1, sizeof(b), f);
  assert(!ferror(f)); assert(fclose(f) == 0); return std::string(b, n);
}
int main() {
  const char *tmp=getenv("TMPDIR");
  const std::string pattern=std::string(tmp ? tmp : "/tmp") + "/3dslibris-prefs-XXXXXX";
  std::vector<char> storage(pattern.begin(), pattern.end()); storage.push_back(0);
  char *dir=storage.data(); assert(mkdtemp(dir));
  const std::string path = std::string(dir) + "/prefs.xml";
  const std::string temp = path + ".tmp", backup = path + ".bak";
  auto save = [&](const char *text) {
    FILE *f = fopen(temp.c_str(), "wb"); assert(f); assert(fputs(text, f) >= 0);
    return prefs_file_utils::Commit(f, path, temp, backup);
  };
  assert(save("first")); assert(save("second"));
  assert(Read(prefs_file_utils::OpenForRead(path, backup)) == "second");
  assert(Read(fopen(backup.c_str(), "rb")) == "first");
  // An incomplete temporary is never used for startup recovery.
  Put(temp, "partial");
  assert(Read(prefs_file_utils::OpenForRead(path, backup)) == "second");
  // A stream write failure cannot replace the existing file.
  FILE *bad = fopen(temp.c_str(), "rb"); assert(bad);
  assert(fputs("bad", bad) == EOF);
  assert(!prefs_file_utils::Commit(bad, path, temp, backup));
  assert(Read(fopen(path.c_str(), "rb")) == "second");
  // Simulate interruption after moving current to backup, before installation.
  assert(remove(backup.c_str()) == 0);
  assert(rename(path.c_str(), backup.c_str()) == 0);
  Put(temp, "partial");
  assert(Read(prefs_file_utils::OpenForRead(path, backup)) == "second");
  assert(save("third"));
  // Force installation failure: temp no longer names the open stream.
  FILE *f = fopen(temp.c_str(), "wb"); assert(f); fputs("fourth", f);
  assert(remove(temp.c_str()) == 0);
  assert(!prefs_file_utils::Commit(f, path, temp, backup));
  assert(Read(prefs_file_utils::OpenForRead(path, backup)) == "third");
  assert(Read(fopen(path.c_str(), "rb")) == "third" && "rollback must restore the primary, not only expose a backup");
  assert(save("fourth"));
  assert(Read(fopen(backup.c_str(), "rb")) == "third");
  // A non-ENOENT primary failure must not silently fall back to stale prefs.
  const std::string not_directory=std::string(dir) + "/not-directory";
  Put(not_directory, "file");
  assert(!prefs_file_utils::OpenForRead(not_directory + "/prefs", backup));
  assert(!prefs_file_utils::Commit(nullptr, path, temp, backup));

  // Refusing a non-regular destination preserves both it and the last backup.
  assert(remove(path.c_str()) == 0 && mkdir(path.c_str(), 0700) == 0);
  assert(!save("refused"));
  struct stat info; assert(stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
  assert(Read(fopen(backup.c_str(), "rb")) == "third");
  assert(rmdir(path.c_str()) == 0);
  assert(save("fifth"));
  // Failure to replace an existing non-empty backup must leave primary intact.
  assert(remove(backup.c_str()) == 0 && mkdir(backup.c_str(), 0700) == 0);
  const std::string blocker=backup + "/block"; Put(blocker, "keep");
  assert(!save("sixth"));
  assert(Read(fopen(path.c_str(), "rb")) == "fifth");
  assert(Read(fopen(blocker.c_str(), "rb")) == "keep");
  assert(remove(blocker.c_str()) == 0 && rmdir(backup.c_str()) == 0);
  assert(save("sixth"));
  assert(Read(fopen(path.c_str(), "rb")) == "sixth");
  assert(Read(fopen(backup.c_str(), "rb")) == "fifth");
  remove(not_directory.c_str());
  remove(temp.c_str()); remove(path.c_str()); remove(backup.c_str()); rmdir(dir);
  puts("PASS: preference replacement, write failure, rollback and recovery");
}
