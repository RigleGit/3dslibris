#include "formats/common/zip_read_utils.h"

#include <climits>

namespace zip_read_utils {
namespace {

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

void SetError(const char **error_message, const char *message) {
  if (error_message)
    *error_message = message;
}

bool ReadOpenedCurrentEntryBinary(unzFile uf,
                                  std::vector<unsigned char> *out,
                                  size_t max_bytes,
                                  size_t reserve_bytes,
                                  const char **error_message) {
  size_t total = 0;
  // Sized entries can inflate straight into their final storage. Keep the
  // streaming tail for callers without metadata or an outdated size hint.
  out->resize(reserve_bytes);
  while (total < reserve_bytes) {
    const int n = unzReadCurrentFile(uf, out->data() + total,
                                     (unsigned int)(reserve_bytes - total));
    if (n < 0) {
      out->clear();
      SetError(error_message, "zip entry read failed");
      return false;
    }
    if (n == 0)
      break;
    total += (size_t)n;
  }
  out->resize(total);

  unsigned char buf[8 * 1024];
  while (true) {
    const int n = unzReadCurrentFile(uf, buf, sizeof(buf));
    if (n < 0) {
      out->clear();
      SetError(error_message, "zip entry read failed");
      return false;
    }
    if (n == 0)
      break;
    if ((size_t)n > max_bytes - total) {
      out->clear();
      SetError(error_message, "zip entry exceeded size limit while reading");
      return false;
    }
    out->insert(out->end(), buf, buf + n);
    total += (size_t)n;
  }

  if (out->empty()) {
    SetError(error_message, "zip entry was empty");
    return false;
  }

  return true;
}

} // namespace

bool ReadCurrentEntryBinary(unzFile uf, unsigned long uncompressed_size,
                            std::vector<unsigned char> *out,
                            size_t max_bytes,
                            const char **error_message) {
  SetError(error_message, "");
  if (!uf || !out) {
    SetError(error_message, "zip entry read parameters invalid");
    return false;
  }

  out->clear();

  if (uncompressed_size == 0 || uncompressed_size > max_bytes ||
      uncompressed_size > (unsigned long)INT_MAX) {
    SetError(error_message, "zip entry rejected by size limits");
    return false;
  }

  ScopedCurrentZipEntry current(uf);
  if (!current.Open()) {
    SetError(error_message, "zip entry open failed");
    return false;
  }

  return ReadOpenedCurrentEntryBinary(uf, out, max_bytes,
                                      (size_t)uncompressed_size,
                                      error_message);
}

bool ReadEntryBinary(unzFile uf, const std::string &path,
                     std::vector<unsigned char> *out,
                     size_t max_bytes,
                     const char **error_message) {
  SetError(error_message, "");
  if (!uf || !out || path.empty() || max_bytes == 0) {
    SetError(error_message, "zip entry read parameters invalid");
    return false;
  }

  out->clear();

  if (unzLocateFile(uf, path.c_str(), 2) != UNZ_OK) {
    SetError(error_message, "zip entry locate failed");
    return false;
  }

  unz_file_info fi = {};
  if (unzGetCurrentFileInfo(uf, &fi, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK) {
    return ReadCurrentEntryBinary(uf, fi.uncompressed_size, out, max_bytes,
                                  error_message);
  }

  ScopedCurrentZipEntry current(uf);
  if (!current.Open()) {
    SetError(error_message, "zip entry open failed");
    return false;
  }

  return ReadOpenedCurrentEntryBinary(uf, out, max_bytes, 0, error_message);
}

} // namespace zip_read_utils
