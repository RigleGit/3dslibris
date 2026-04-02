/*
    3dslibris - epub_zip_utils.cpp
    ZIP archive utilities for EPUB parsing.
    Extracted from epub.cpp by Rigle.
*/

#include "formats/epub/epub_zip_utils.h"

#include "minizip/unzip.h"
#include "string_utils.h"
#include <string.h>

namespace epub_zip_utils {

bool LocateSafe(unzFile uf, const std::string &entry_path,
                ZipEntryIndex *index) {
  if (!uf || entry_path.empty())
    return false;

  if (index && !index->built) {
    index->built = true;
    int rc = unzGoToFirstFile(uf);
    if (rc == UNZ_OK) {
      do {
        char fname[1024];
        unz_file_info fi;
        if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL,
                                  0) == UNZ_OK) {
          std::string key = ToLowerAscii(NormalizeZipEntryName(fname));
          if (!key.empty() && index->offset_by_key.find(key) ==
                                  index->offset_by_key.end()) {
            index->offset_by_key[key] = unzGetOffset(uf);
          }
        }
        rc = unzGoToNextFile(uf);
      } while (rc == UNZ_OK);
    }
  }

  if (index && !index->offset_by_key.empty()) {
    std::string wanted = ToLowerAscii(NormalizeZipEntryName(entry_path));
    auto hit = index->offset_by_key.find(wanted);
    if (hit != index->offset_by_key.end()) {
      if (unzSetOffset(uf, hit->second) == UNZ_OK)
        return true;
    }
  }

  if (unzLocateFile(uf, entry_path.c_str(), 0) == UNZ_OK)
    return true;

  std::string wanted = NormalizeZipEntryName(entry_path);
  int rc = unzGoToFirstFile(uf);
  if (rc != UNZ_OK)
    return false;

  size_t scanned = 0;
  do {
    scanned++;
    unz_file_info fi;
    char fname[1024];
    int info_rc =
        unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL, 0);
    if (info_rc == UNZ_OK) {
      std::string current = NormalizeZipEntryName(std::string(fname));
      if (current == wanted || EqualsAsciiNoCase(current, wanted))
        return true;
    }
    if (scanned > 32768)
      break;
    rc = unzGoToNextFile(uf);
  } while (rc == UNZ_OK);

  return false;
}

bool ReadText(unzFile uf, const std::string &path, std::string &out,
              size_t max_bytes, ZipEntryIndex *index) {
  out.clear();
  if (path.empty())
    return false;

  if (!LocateSafe(uf, path, index))
    return false;

  unz_file_info fi;
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
    return false;

  if (fi.uncompressed_size > max_bytes)
    return false;

  int rc = unzOpenCurrentFile(uf);
  if (rc != UNZ_OK)
    return false;

  std::string buf;
  buf.resize(fi.uncompressed_size);
  int total = 0;
  while (total < (int)buf.size()) {
    int n = unzReadCurrentFile(uf, &buf[0] + total,
                               (unsigned int)(buf.size() - (size_t)total));
    if (n < 0) {
      unzCloseCurrentFile(uf);
      return false;
    }
    if (n == 0)
      break;
    total += n;
  }
  unzCloseCurrentFile(uf);
  if (total <= 0)
    return false;

  buf.resize((size_t)total);
  buf.push_back('\0');
  out.swap(buf);
  if (!out.empty() && out.back() == '\0')
    out.pop_back();
  return true;
}

} // namespace epub_zip_utils
