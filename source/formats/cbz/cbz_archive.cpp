#include "formats/cbz/cbz_archive.h"
#include "shared/fixed_layout_perf.h"

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

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool NaturalPathLess(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (IsAsciiDigit(a[i]) && IsAsciiDigit(b[j])) {
      size_t a_end = i, b_end = j;
      while (a_end < a.size() && IsAsciiDigit(a[a_end]))
        a_end++;
      while (b_end < b.size() && IsAsciiDigit(b[b_end]))
        b_end++;
      size_t a_digits = i, b_digits = j;
      while (a_digits < a_end && a[a_digits] == '0')
        a_digits++;
      while (b_digits < b_end && b[b_digits] == '0')
        b_digits++;

      // Compare arbitrary-length numbers without conversion or allocation.
      if (a_end - a_digits != b_end - b_digits)
        return a_end - a_digits < b_end - b_digits;
      for (size_t n = 0; n < a_end - a_digits; n++) {
        if (a[a_digits + n] != b[b_digits + n])
          return a[a_digits + n] < b[b_digits + n];
      }
      // Equal values: fewer leading zeroes first. Resolve this before the
      // rest of the path so distinct directory names stay grouped together.
      if (a_end - i != b_end - j)
        return a_end - i < b_end - j;
      i = a_end;
      j = b_end;
    } else {
      if (a[i] != b[j])
        return (unsigned char)a[i] < (unsigned char)b[j];
      i++;
      j++;
    }
  }
  return i == a.size() && j < b.size();
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

void ReadCurrentComicInfo(unzFile uf, std::vector<CbzComicInfoBookmark> *out) {
  ScopedCurrentZipEntry current(uf);
  if (!current.Open())
    return;

  ComicInfoParseState state;
  state.out = out;
  state.in_pages = false;

  xml_parse_utils::XmlParserOptions opts;
  opts.start_element = ComicInfoStartElement;
  opts.end_element = ComicInfoEndElement;
  opts.user_data = &state;
  xml_parse_utils::ParseXmlZipEntry(uf, opts, 4096);
}

} // namespace

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries) {
  CbzArchiveReader reader;
  return reader.Index(archive_path, entries);
}

bool CbzArchiveReader::Index(const std::string &archive_path,
                             std::vector<CbzPageEntry> *entries,
                             std::vector<CbzComicInfoBookmark> *bookmarks) {
  Close();
  if (bookmarks)
    bookmarks->clear();
  if (entries)
    entries->clear();
  if (!entries || archive_path.empty())
    return false;

  SetLastCbzArchiveError("");

  archive_ = unzOpen(archive_path.c_str());
  if (!archive_) {
    SetLastCbzArchiveError("unable to open CBZ archive");
    return false;
  }
  path_ = archive_path;

  bool comic_info_checked = false;
  ForEachZipEntry(archive_, [&](const ZipEntryInfo &zip_entry) {
    const std::string &normalized = zip_entry.normalized_name;
    if (bookmarks && !comic_info_checked && IsComicInfoXmlEntry(normalized) &&
        zip_entry.file_info.uncompressed_size > 0 &&
        zip_entry.file_info.uncompressed_size < 2u * 1024u * 1024u) {
      comic_info_checked = true;
      ReadCurrentComicInfo(archive_, bookmarks);
    }

    if (!normalized.empty() && normalized[normalized.size() - 1] != '/' &&
        IsSupportedCbzImagePath(normalized)) {
      CbzPageEntry entry;
      entry.path = zip_entry.raw_name;
      entry.normalized_path = normalized;
      entry.offset = unzGetOffset(archive_);
      entry.uncompressed_size = zip_entry.file_info.uncompressed_size;
      entries->push_back(entry);
    }

    return false;
  });

  std::sort(entries->begin(), entries->end(),
            [](const CbzPageEntry &a, const CbzPageEntry &b) {
              return NaturalPathLess(a.normalized_path, b.normalized_path);
            });

  if (entries->empty()) {
    SetLastCbzArchiveError("no supported image entries found in CBZ");
    if (bookmarks)
      bookmarks->clear();
    Close();
    return false;
  }

  if (bookmarks)
    std::sort(bookmarks->begin(), bookmarks->end(),
              [](const CbzComicInfoBookmark &a, const CbzComicInfoBookmark &b) {
                return a.image_index < b.image_index;
              });
  return true;
}

CbzArchiveReader::CbzArchiveReader() : archive_(NULL), path_() {}

CbzArchiveReader::~CbzArchiveReader() { Close(); }

void CbzArchiveReader::Swap(CbzArchiveReader &other) {
  std::swap(archive_, other.archive_);
  path_.swap(other.path_);
}

void CbzArchiveReader::Close() {
  if (archive_)
    unzClose(archive_);
  archive_ = NULL;
  path_.clear();
}

bool CbzArchiveReader::Read(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes) {
  if (!out || archive_path.empty() || entry.path.empty())
    return false;

  out->clear();
  SetLastCbzArchiveError("");

  // Entry identity is separate from the viewer identity. Preserve both names
  // so reads for covers/prefetch can also be attributed without global state.
  fixed_perf::Document(&entry, "CBZ_ZIP", archive_path.c_str());
  fixed_perf::Document(&entry, "CBZ_ENTRY", entry.path.c_str());
  if (archive_ && path_ != archive_path)
    Close();
  if (!archive_) {
    fixed_perf::Timer perf_open(&entry, -1, -1, "cbz.zip_open");
    archive_ = unzOpen(archive_path.c_str());
    perf_open.End(archive_ ? 1 : 0);
    if (!archive_) {
      SetLastCbzArchiveError("unable to reopen CBZ archive");
      return false;
    }
    path_ = archive_path;
  } else {
    fixed_perf::Record(&entry, -1, -1, "cbz.zip_reuse", 0, 1);
  }

  const auto read_entry = [&](bool indexed_size) {
    fixed_perf::Timer perf_read(&entry, -1, -1, "cbz.zip_read_inflate");
    const bool ok = indexed_size
        ? ReadCurrentZipEntry(archive_, entry.uncompressed_size, out, max_bytes)
        : ReadLocatedZipEntry(archive_, out, max_bytes);
    perf_read.End(ok ? 1 : 0, out->size());
    return ok;
  };
  if (entry.offset != 0) {
    fixed_perf::Timer perf_locate(&entry, -1, -1, "cbz.zip_locate_offset");
    const int rc = unzSetOffset(archive_, entry.offset);
    perf_locate.End(rc == UNZ_OK ? 1 : 0);
    if (rc == UNZ_OK && read_entry(true))
      return true;
  }

  fixed_perf::Timer perf_locate(&entry, -1, -1, "cbz.zip_locate_name");
  const int rc = unzLocateFile(archive_, entry.path.c_str(), 2);
  perf_locate.End(rc == UNZ_OK ? 1 : 0);
  if (rc != UNZ_OK) {
    SetLastCbzArchiveError("zip entry locate failed");
    Close();
    return false;
  }

  const bool ok = read_entry(false);
  if (!ok)
    Close();
  else
    SetLastCbzArchiveError("");
  return ok;
}

bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes) {
  CbzArchiveReader reader;
  return reader.Read(archive_path, entry, out, max_bytes);
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

    ReadCurrentComicInfo(archive.uf, out);
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
