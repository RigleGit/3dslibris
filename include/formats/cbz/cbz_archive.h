#pragma once

#include "formats/cbz/cbz_types.h"

#include <string>
#include <vector>

struct CbzComicInfoBookmark {
  int image_index;
  std::string title;

  CbzComicInfoBookmark() : image_index(-1), title() {}
};

bool IndexCbzArchiveEntries(const std::string &archive_path,
                            std::vector<CbzPageEntry> *entries);
bool ReadCbzArchiveEntryBytes(const std::string &archive_path,
                              const CbzPageEntry &entry,
                              std::vector<unsigned char> *out,
                              size_t max_bytes);
bool ReadComicInfoBookmarks(const std::string &archive_path,
                            std::vector<CbzComicInfoBookmark> *out);
const char *GetLastCbzArchiveError();

// One reader per owner/thread. Entries are closed after each read; the archive
// remains open until Close(), an error, or destruction. Never share its cursor.
class CbzArchiveReader {
public:
  CbzArchiveReader();
  ~CbzArchiveReader();
  void Close();
  // Transfer the retained archive together with its path identity.
  void Swap(CbzArchiveReader &other);
  bool Index(const std::string &archive_path, std::vector<CbzPageEntry> *entries,
             std::vector<CbzComicInfoBookmark> *bookmarks = NULL);
  bool Read(const std::string &archive_path, const CbzPageEntry &entry,
            std::vector<unsigned char> *out, size_t max_bytes);

private:
  void *archive_;
  std::string path_;
  CbzArchiveReader(const CbzArchiveReader &);
  CbzArchiveReader &operator=(const CbzArchiveReader &);
};
