#include "settings/cache_cleanup_utils.h"

#include "test_assert.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static void WriteFile(const std::string &path) {
  FILE *file = fopen(path.c_str(), "wb");
  test::ExpectNotNull("create cache fixture", file);
  fputs("cache", file);
  fclose(file);
}

static bool Exists(const std::string &path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

int main() {
  char dir_template[] = "/tmp/3dslibris-cache-cleanup-XXXXXX";
  char *dir = mkdtemp(dir_template);
  test::ExpectNotNull("create temporary cache directory", dir);

  const std::string base(dir);
  WriteFile(base + "/page.cache");
  WriteFile(base + "/.keep");

  const char *nfc_name = "caf\xC3\xA9.cache";
  const char *nfd_name = "cafe\xCC\x81.cache";
  WriteFile(base + "/" + nfc_name);

  settings::CacheCleanupResult result =
      settings::DeleteCacheDirectoryContents(base.c_str());
  test::ExpectTrue("cache directory opened", result.opened);
  test::ExpectEq("normal and unicode cache files removed", result.removed, 2);
  test::ExpectEq("cache cleanup has no failures", result.failed, 0);
  test::ExpectFalse("normal cache file deleted", Exists(base + "/page.cache"));
  test::ExpectTrue("hidden file preserved", Exists(base + "/.keep"));

  WriteFile(base + "/" + nfc_name);
  test::ExpectTrue(
      "NFD entry removes NFC filesystem name",
      settings::RemoveCacheEntry(base.c_str(), nfd_name));
  test::ExpectFalse("normalized cache file deleted",
                    Exists(base + "/" + nfc_name));

  result = settings::DeleteCacheDirectoryContents(
      "/tmp/3dslibris-cache-cleanup-does-not-exist");
  test::ExpectFalse("missing cache directory not opened", result.opened);
  test::ExpectEq("missing cache directory removes nothing", result.removed, 0);

  // A full cache directory (page caches, stream scratch, metadata) is
  // emptied in one call.
  for (int i = 0; i < 64; i++) {
    char name[64];
    snprintf(name, sizeof(name), "/%08x.cache", i * 2654435761u);
    WriteFile(base + name);
  }
  WriteFile(base + "/794210e7.stream.txt");
  result = settings::DeleteCacheDirectoryContents(base.c_str());
  test::ExpectEq("every cache file removed in one pass", result.removed, 65);
  test::ExpectEq("full cleanup has no failures", result.failed, 0);
  int left = 0;
  DIR *listing = opendir(base.c_str());
  test::ExpectNotNull("reopen cache directory", listing);
  while (struct dirent *e = readdir(listing))
    if (e->d_name[0] != '.')
      left++;
  closedir(listing);
  test::ExpectEq("no cache file left behind", left, 0);

  remove((base + "/.keep").c_str());
  rmdir(base.c_str());
  return 0;
}
