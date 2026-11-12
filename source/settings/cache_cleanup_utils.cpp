#include "settings/cache_cleanup_utils.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "utf8proc.h"

namespace settings {

bool RemoveCacheEntry(const char *directory, const char *name) {
  if (!directory || !name)
    return false;

  uint8_t *nfc = nullptr;
  const utf8proc_ssize_t nfc_len = utf8proc_map(
      reinterpret_cast<const uint8_t *>(name), 0, &nfc,
      static_cast<utf8proc_option_t>(UTF8PROC_NULLTERM | UTF8PROC_STABLE |
                                    UTF8PROC_COMPOSE));
  const char *safe_name =
      (nfc_len >= 0 && nfc) ? reinterpret_cast<const char *>(nfc) : name;

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", directory, safe_name);
  const bool removed = remove(path) == 0;
  free(nfc);
  return removed;
}

CacheCleanupResult DeleteCacheDirectoryContents(const char *directory) {
  CacheCleanupResult result;
  if (!directory)
    return result;

  DIR *dir = opendir(directory);
  if (!dir)
    return result;
  result.opened = true;

  // List first and delete after closing the directory: removing entries
  // while the SD card directory is being read can make it skip files.
  std::vector<std::string> names;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (entry->d_name[0] == '.')
      continue;
    names.push_back(entry->d_name);
  }
  closedir(dir);

  for (size_t i = 0; i < names.size(); i++) {
    if (RemoveCacheEntry(directory, names[i].c_str()))
      result.removed++;
    else
      result.failed++;
  }
  return result;
}

} // namespace settings
