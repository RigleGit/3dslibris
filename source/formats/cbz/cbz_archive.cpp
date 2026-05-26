#include "formats/cbz/cbz_archive.h"

#include "formats/common/zip_read_utils.h"
#include "formats/common/xml_parse_utils.h"
#include "minizip/unzip.h"
#include "shared/string_utils.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

static char g_last_cbz_archive_error[192] = "";

void SetLastCbzArchiveError(const char *message) {
  if (!message)
    message = "";

  std::snprintf(g_last_cbz_archive_error, sizeof(g_last_cbz_archive_error),
                "%s", message);
}

struct ScopedZipArchive {
  explicit ScopedZipArchive(const std::string &path)
      : uf(unzOpen(path.c_str())) {}

  ~ScopedZipArchive() {
    if (uf)
      unzClose(uf);
  }

  bool ok() const {
    return uf != NULL;
  }

  unzFile uf;

private:
  ScopedZipArchive(const ScopedZipArchive &);
  ScopedZipArchive &operator=(const ScopedZipArchive &);
};

struct ScopedCurrentZipEntry {
  explicit ScopedCurrentZipEntry(unzFile file) : uf(file), opened(false) {}

  bool Open() {
    opened = (unzOpenCurrentFile(uf) == UNZ_OK);
    return opened;
  }

  ~ScopedCurrentZipEntry() {
    if (opened)
      unzCloseCurrentFile(uf);
  }

  unzFile uf;
  bool opened;

private:
  ScopedCurrentZipEntry(const ScopedCurrentZipEntry &);
  ScopedCurrentZipEntry &operator=(const ScopedCurrentZipEntry &);
};

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
  const char *name = path.c_str();

  return HasExtCI(name, ".jpg") ||
         HasExtCI(name, ".jpeg") ||
         HasExtCI(name, ".png");
}

struct ZipEntryInfo {
  std::string raw_name;
  std::string normalized_name;
  unz_file_info file_info;
};

template <typename Callback>
void ForEachZipEntry(unzFile uf, Callback callback) {
  if (!uf)
    return;

  int rc = unzGoToFirstFile(uf);
  while (rc == UNZ_OK) {
    char fname[1024] = {};
    unz_file_info fi = {};

    if (unzGetCurrentFileInfo(uf, &fi, fname, sizeof(fname), NULL, 0, NULL,
                              0) == UNZ_OK) {
      fname[sizeof(fname) - 1] = '\0';

      ZipEntryInfo entry;
      entry.raw_name = fname;
      entry.normalized_name = NormalizeCbzEntryName(entry.raw_name);
      entry.file_info = fi;

      if (callback(entry))
        break;
    }

    rc = unzGoToNextFile(uf);
  }
}

bool ReadCurrentZipEntry(unzFile uf, unsigned long uncompressed_size,
                         std::vector<unsigned char> *out, size_t max_bytes) {
  const char *error_message = NULL;
  if (!zip_read_utils::ReadCurrentEntryBinary(
          uf, uncompressed_size, out, max_bytes, &error_message)) {
    SetLastCbzArchiveError(error_message);
    return false;
  }
  return true;
}

bool ReadLocatedZipEntry(unzFile uf, std::vector<unsigned char> *out,
                         size_t max_bytes) {
  unz_file_info fi = {};

  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK) {
    SetLastCbzArchiveError("zip entry info read failed");
    return false;
  }

  return ReadCurrentZipEntry(uf, fi.uncompressed_size, out, max_bytes);
}

bool IsComicInfoXmlEntry(const std::string &normalized_path) {
  const std::string lower = ToLowerAscii(normalized_path);
  const std::string suffix = "comicinfo.xml";

  if (lower == suffix)
    return true;

  if (lower.size() <= suffix.size())
    return false;

  const size_t suffix_pos = lower.size() - suffix.size();

  return lower.compare(suffix_pos, suffix.size(), suffix) == 0 &&
         lower[suffix_pos - 1] == '/';
}

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

} // namespace

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries) {
  if (!entries || archive_path.empty())
    return false;

  entries->clear();
  SetLastCbzArchiveError("");

  ScopedZipArchive archive(archive_path);
  if (!archive.ok()) {
    SetLastCbzArchiveError("unable to open CBZ archive");
    return false;
  }

  ForEachZipEntry(archive.uf, [&](const ZipEntryInfo &zip_entry) {
    const std::string &normalized = zip_entry.normalized_name;

    if (!normalized.empty() && normalized[normalized.size() - 1] != '/' &&
        IsSupportedCbzImagePath(normalized)) {
      CbzPageEntry entry;
      entry.path = zip_entry.raw_name;
      entry.normalized_path = normalized;
      entry.offset = unzGetOffset(archive.uf);
      entry.uncompressed_size = zip_entry.file_info.uncompressed_size;
      entries->push_back(entry);
    }

    return false;
  });

  std::sort(entries->begin(), entries->end(),
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

  ScopedZipArchive archive(archive_path);
  if (!archive.ok()) {
    SetLastCbzArchiveError("unable to reopen CBZ archive");
    return false;
  }

  if (entry.offset != 0 && unzSetOffset(archive.uf, entry.offset) == UNZ_OK) {
    if (ReadLocatedZipEntry(archive.uf, out, max_bytes))
      return true;
  }

  if (unzLocateFile(archive.uf, entry.path.c_str(), 2) != UNZ_OK) {
    SetLastCbzArchiveError("zip entry locate failed");
    return false;
  }

  return ReadLocatedZipEntry(archive.uf, out, max_bytes);
}

const char *GetLastCbzArchiveError() {
  return g_last_cbz_archive_error;
}

bool ReadComicInfoBookmarks(const std::string &archive_path,
                            std::vector<CbzComicInfoBookmark> *out) {
  if (!out || archive_path.empty())
    return false;

  out->clear();

  ScopedZipArchive archive(archive_path);
  if (!archive.ok())
    return false;

  bool found = false;

  ForEachZipEntry(archive.uf, [&](const ZipEntryInfo &zip_entry) {
    const unz_file_info &fi = zip_entry.file_info;

    if (!IsComicInfoXmlEntry(zip_entry.normalized_name))
      return false;

    if (fi.uncompressed_size == 0 || fi.uncompressed_size >= 2u * 1024u * 1024u)
      return false;

    ScopedCurrentZipEntry current(archive.uf);
    if (!current.Open())
      return true;

    ComicInfoParseState state;
    state.out = out;
    state.in_pages = false;

    xml_parse_utils::XmlParserOptions opts;
    opts.start_element = ComicInfoStartElement;
    opts.end_element = ComicInfoEndElement;
    opts.user_data = &state;

    xml_parse_utils::ParseXmlZipEntry(archive.uf, opts, 4096);

    found = true;
    return true;
  });

  if (!found || out->empty())
    return false;

  std::sort(out->begin(), out->end(),
            [](const CbzComicInfoBookmark &a, const CbzComicInfoBookmark &b) {
              return a.image_index < b.image_index;
            });

  return true;
}
