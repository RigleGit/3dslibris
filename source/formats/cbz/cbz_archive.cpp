#include "formats/cbz/cbz_archive.h"

#include "minizip/unzip.h"
#include "string_utils.h"

#include <algorithm>
#include <climits>

namespace {

std::string NormalizeCbzEntryName(const std::string &name) {
  std::string out = name;
  std::replace(out.begin(), out.end(), '\\', '/');
  while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
    out.erase(0, 2);
  while (!out.empty() && out[0] == '/')
    out.erase(out.begin());
  return out;
}

bool IsSupportedCbzImagePath(const std::string &path) {
  const std::string lower = ToLowerAscii(path);
  return HasExtCI(lower.c_str(), ".jpg") || HasExtCI(lower.c_str(), ".jpeg") ||
         HasExtCI(lower.c_str(), ".png");
}

bool ReadCurrentZipEntry(unzFile uf, unsigned long uncompressed_size,
                         std::vector<unsigned char> *out, size_t max_bytes) {
  if (!uf || !out)
    return false;
  out->clear();
  if (uncompressed_size == 0 || uncompressed_size > max_bytes ||
      uncompressed_size > (unsigned long)INT_MAX) {
    return false;
  }
  out->reserve((size_t)uncompressed_size);

  if (unzOpenCurrentFile(uf) != UNZ_OK)
    return false;

  unsigned char buf[8 * 1024];
  int n = 0;
  size_t total = 0;
  while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
    if (total + (size_t)n > max_bytes) {
      unzCloseCurrentFile(uf);
      out->clear();
      return false;
    }
    out->insert(out->end(), buf, buf + n);
    total += (size_t)n;
  }

  unzCloseCurrentFile(uf);
  if (n < 0 || out->empty()) {
    out->clear();
    return false;
  }
  return true;
}

} // namespace

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries) {
  if (!entries || archive_path.empty())
    return false;
  entries->clear();

  unzFile uf = unzOpen(archive_path.c_str());
  if (!uf)
    return false;

  int rc = unzGoToFirstFile(uf);
  while (rc == UNZ_OK) {
    char fname[1024];
    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL, 0) ==
        UNZ_OK) {
      std::string normalized = NormalizeCbzEntryName(fname);
      if (!normalized.empty() && normalized[normalized.size() - 1] != '/' &&
          IsSupportedCbzImagePath(normalized)) {
        CbzPageEntry entry;
        entry.path = fname;
        entry.normalized_path = normalized;
        entry.offset = unzGetOffset(uf);
        entry.uncompressed_size = fi.uncompressed_size;
        entries->push_back(entry);
      }
    }
    rc = unzGoToNextFile(uf);
  }

  unzClose(uf);

  std::stable_sort(entries->begin(), entries->end(),
                   [](const CbzPageEntry &a, const CbzPageEntry &b) {
                     return a.normalized_path < b.normalized_path;
                   });
  return !entries->empty();
}

bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes) {
  if (!out || archive_path.empty() || entry.path.empty())
    return false;
  out->clear();

  unzFile uf = unzOpen(archive_path.c_str());
  if (!uf)
    return false;

  bool ok = false;
  if (entry.offset != 0 && unzSetOffset(uf, entry.offset) == UNZ_OK) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK) {
      ok = ReadCurrentZipEntry(uf, fi.uncompressed_size, out, max_bytes);
    }
  }

  if (!ok && unzLocateFile(uf, entry.path.c_str(), 2) == UNZ_OK) {
    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK)
      ok = ReadCurrentZipEntry(uf, fi.uncompressed_size, out, max_bytes);
  }

  unzClose(uf);
  return ok;
}
