#include "formats/cbz/cbz_archive.h"

#include "formats/common/xml_parse_utils.h"
#include "minizip/unzip.h"
#include "shared/string_utils.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace {

static char g_last_cbz_archive_error[192] = "";

void SetLastCbzArchiveError(const char *message) {
  if (!message)
    message = "";
  std::snprintf(g_last_cbz_archive_error, sizeof(g_last_cbz_archive_error),
                "%s", message);
}

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
    SetLastCbzArchiveError("zip entry rejected by size limits");
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
      SetLastCbzArchiveError("zip entry exceeded size limit while reading");
      return false;
    }
    out->insert(out->end(), buf, buf + n);
    total += (size_t)n;
  }

  unzCloseCurrentFile(uf);
  if (n < 0 || out->empty()) {
    out->clear();
    SetLastCbzArchiveError(n < 0 ? "zip entry read failed"
                                 : "zip entry was empty");
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
  SetLastCbzArchiveError("");

  unzFile uf = unzOpen(archive_path.c_str());
  if (!uf) {
    SetLastCbzArchiveError("unable to open CBZ archive");
    return false;
  }

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
  if (entries->empty()) {
    SetLastCbzArchiveError("no supported image entries found in CBZ");
    return false;
  }
  return true;
}

bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes) {
  if (!out || archive_path.empty() || entry.path.empty())
    return false;
  out->clear();
  SetLastCbzArchiveError("");

  unzFile uf = unzOpen(archive_path.c_str());
  if (!uf) {
    SetLastCbzArchiveError("unable to reopen CBZ archive");
    return false;
  }

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
  } else if (!ok) {
    SetLastCbzArchiveError("zip entry locate failed");
  }

  unzClose(uf);
  return ok;
}

const char *GetLastCbzArchiveError() {
  return g_last_cbz_archive_error;
}

namespace {

struct ComicInfoParseState {
  std::vector<CbzComicInfoBookmark> *out;
  bool in_pages;
};

static void XMLCALL ComicInfoStartElement(void *user_data, const XML_Char *name,
                                          const XML_Char **atts) {
  ComicInfoParseState *state = static_cast<ComicInfoParseState *>(user_data);
  if (strcmp(name, "Pages") == 0) {
    state->in_pages = true;
    return;
  }
  if (!state->in_pages || strcmp(name, "Page") != 0)
    return;

  int image_index = -1;
  const char *bookmark = NULL;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "Image") == 0) {
      image_index = atoi(atts[i + 1]);
    } else if (strcmp(atts[i], "Bookmark") == 0 && atts[i + 1][0] != '\0') {
      bookmark = atts[i + 1];
    }
  }
  if (image_index >= 0 && bookmark) {
    CbzComicInfoBookmark entry;
    entry.image_index = image_index;
    entry.title = bookmark;
    state->out->push_back(entry);
  }
}

static void XMLCALL ComicInfoEndElement(void *user_data, const XML_Char *name) {
  ComicInfoParseState *state = static_cast<ComicInfoParseState *>(user_data);
  if (strcmp(name, "Pages") == 0)
    state->in_pages = false;
}

bool IsComicInfoXmlEntry(const std::string &normalized_path) {
  const std::string lower = ToLowerAscii(normalized_path);
  const std::string suffix = "comicinfo.xml";
  if (lower.size() < suffix.size())
    return false;
  const std::string tail = lower.substr(lower.size() - suffix.size());
  if (tail != suffix)
    return false;
  const size_t base_start = lower.size() - suffix.size();
  return base_start == 0 || lower[base_start - 1] == '/';
}

} // namespace

bool ReadComicInfoBookmarks(const std::string &archive_path,
                            std::vector<CbzComicInfoBookmark> *out) {
  if (!out || archive_path.empty())
    return false;
  out->clear();

  unzFile uf = unzOpen(archive_path.c_str());
  if (!uf)
    return false;

  bool found = false;
  int rc = unzGoToFirstFile(uf);
  while (rc == UNZ_OK) {
    char fname[1024];
    unz_file_info fi;
    if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL,
                              0) == UNZ_OK) {
      std::string normalized = NormalizeCbzEntryName(fname);
      if (IsComicInfoXmlEntry(normalized) && fi.uncompressed_size > 0 &&
          fi.uncompressed_size < 2u * 1024u * 1024u) {
        if (unzOpenCurrentFile(uf) == UNZ_OK) {
          ComicInfoParseState state;
          state.out = out;
          state.in_pages = false;

          xml_parse_utils::XmlParserOptions opts;
          opts.start_element = ComicInfoStartElement;
          opts.end_element = ComicInfoEndElement;
          opts.user_data = &state;

          xml_parse_utils::ParseXmlZipEntry(uf, opts, 4096);
          unzCloseCurrentFile(uf);
          found = true;
        }
        break;
      }
    }
    rc = unzGoToNextFile(uf);
  }

  unzClose(uf);
  if (!found || out->empty())
    return false;

  std::stable_sort(out->begin(), out->end(),
                   [](const CbzComicInfoBookmark &a,
                      const CbzComicInfoBookmark &b) {
                     return a.image_index < b.image_index;
                   });
  return true;
}
