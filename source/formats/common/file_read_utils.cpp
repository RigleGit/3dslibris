#include "formats/common/file_read_utils.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

namespace file_read_utils {
namespace {

struct FileReadContext {
  FILE *fp;
};

static int g_last_error_number = 0;
static char g_last_error_operation[16] = "";
static char g_last_error_path[256] = "";

void SetLastError(const char *operation, const char *path, int err) {
  g_last_error_number = err;
  snprintf(g_last_error_operation, sizeof(g_last_error_operation), "%s",
           operation ? operation : "");
  snprintf(g_last_error_path, sizeof(g_last_error_path), "%s",
           path ? path : "");
}

void ClearLastError() {
  SetLastError("", "", 0);
}

// Prefer an exact-size read when stat() is trustworthy so large books do not
// depend on short-read behavior from the underlying SD/filesystem driver.
bool ReadExactSize(FILE *fp, std::string *out, size_t expected) {
  if (!fp || !out)
    return false;

  out->assign(expected, '\0');
  size_t total = 0;
  while (total < expected) {
    size_t n = fread(&(*out)[total], 1, expected - total, fp);
    if (n == 0) {
      out->clear();
      if (ferror(fp))
        SetLastError("fread", "", errno);
      return false;
    }
    total += n;
  }

  if (ferror(fp)) {
    out->clear();
    SetLastError("fread", "", errno);
    return false;
  }
  return true;
}

size_t ReadFileChunk(void *ctx, char *buf, size_t cap) {
  FileReadContext *file = static_cast<FileReadContext *>(ctx);
  if (!file || !file->fp || !buf || cap == 0)
    return 0;
  return fread(buf, 1, cap, file->fp);
}

bool IsFileEof(void *ctx) {
  FileReadContext *file = static_cast<FileReadContext *>(ctx);
  return file && file->fp && feof(file->fp);
}

bool HasFileError(void *ctx) {
  FileReadContext *file = static_cast<FileReadContext *>(ctx);
  return !file || !file->fp || ferror(file->fp);
}

} // namespace

bool ReadAllToString(ReadChunkFn read_chunk, StreamFlagFn is_eof,
                     StreamFlagFn has_error, void *ctx, std::string *out,
                     size_t max_bytes, size_t chunk_size) {
  if (!read_chunk || !is_eof || !has_error || !out || chunk_size == 0)
    return false;

  out->clear();
  std::vector<char> buf(chunk_size);
  while (true) {
    size_t n = read_chunk(ctx, buf.data(), buf.size());
    if (n > 0) {
      if (out->size() + n > max_bytes)
        return false;
      out->append(buf.data(), n);
    }

    if (has_error(ctx))
      return false;

    // Short reads can happen under SD load; only real EOF should stop the loop.
    if (is_eof(ctx))
      return true;

    if (n == 0)
      return false;
  }
}

bool ReadFileToStringLimited(FILE *fp, std::string *out, size_t max_bytes) {
  if (!fp)
    return false;

  FileReadContext ctx = {fp};
  bool ok = ReadAllToString(&ReadFileChunk, &IsFileEof, &HasFileError, &ctx,
                            out, max_bytes, 4096);
  if (!ok && ferror(fp))
    SetLastError("fread", "", errno);
  return ok;
}

bool ReadPathToStringLimited(const char *path, std::string *out,
                             size_t max_bytes) {
  if (!path || !out)
    return false;

  ClearLastError();
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    SetLastError("fopen", path, errno);
    return false;
  }

  bool ok = false;
  struct stat st;
  if (stat(path, &st) == 0 && st.st_size >= 0) {
    size_t expected = (size_t)st.st_size;
    if (expected <= max_bytes)
      ok = ReadExactSize(fp, out, expected);
  }

  if (!ok) {
    // Fall back to EOF-driven reads when stat() is missing or the exact-size
    // path could not complete cleanly.
    clearerr(fp);
    fseek(fp, 0, SEEK_SET);
    ok = ReadFileToStringLimited(fp, out, max_bytes);
  }

  fclose(fp);
  return ok;
}

bool ReadPathToStringLimited(const std::string &path, std::string *out,
                             size_t max_bytes) {
  if (path.empty())
    return false;
  return ReadPathToStringLimited(path.c_str(), out, max_bytes);
}

int LastErrorNumber() {
  return g_last_error_number;
}

const char *LastErrorOperation() {
  return g_last_error_operation;
}

const char *LastErrorPath() {
  return g_last_error_path;
}

} // namespace file_read_utils
