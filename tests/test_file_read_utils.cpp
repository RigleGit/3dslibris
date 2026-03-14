#include "file_read_utils.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEqual(const char *label, const std::string &actual,
                 const std::string &expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected contents");
}

struct ScriptedReader {
  std::string source;
  std::vector<size_t> chunk_sizes;
  size_t cursor;
  size_t read_index;
  bool eof;
  bool error;
};

size_t ReadChunk(void *ctx, char *buf, size_t cap) {
  ScriptedReader *reader = static_cast<ScriptedReader *>(ctx);
  if (!reader || !buf || cap == 0 || reader->cursor >= reader->source.size()) {
    if (reader)
      reader->eof = true;
    return 0;
  }

  size_t limit = cap;
  if (reader->read_index < reader->chunk_sizes.size())
    limit = reader->chunk_sizes[reader->read_index++];

  size_t remaining = reader->source.size() - reader->cursor;
  size_t n = limit < remaining ? limit : remaining;
  if (n > cap)
    n = cap;
  memcpy(buf, reader->source.data() + reader->cursor, n);
  reader->cursor += n;
  reader->eof = reader->cursor >= reader->source.size();
  return n;
}

bool IsEof(void *ctx) {
  ScriptedReader *reader = static_cast<ScriptedReader *>(ctx);
  return reader && reader->eof;
}

bool HasError(void *ctx) {
  ScriptedReader *reader = static_cast<ScriptedReader *>(ctx);
  return !reader || reader->error;
}

} // namespace

int main() {
  using file_read_utils::ReadAllToString;
  using file_read_utils::ReadPathToStringLimited;

  {
    ScriptedReader reader = {"ABCDEFGHIJ", {2, 1, 4, 3}, 0, 0, false, false};
    std::string out;
    ExpectTrue("short reads continue until eof",
               ReadAllToString(&ReadChunk, &IsEof, &HasError, &reader, &out, 64,
                               8));
    ExpectEqual("short reads keep full contents", out, "ABCDEFGHIJ");
  }

  {
    ScriptedReader reader = {"ABCDEFGHIJ", {5, 5}, 0, 0, false, false};
    std::string out;
    ExpectFalse("read respects max bytes",
                ReadAllToString(&ReadChunk, &IsEof, &HasError, &reader, &out, 8,
                                8));
  }

  {
    ScriptedReader reader = {"ABCDEFGHIJ", {0}, 0, 0, false, false};
    std::string out;
    ExpectFalse("zero-byte read without eof fails",
                ReadAllToString(&ReadChunk, &IsEof, &HasError, &reader, &out, 64,
                                8));
  }

  {
    char path[] = "/tmp/3dslibris-read-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
      Fail("mkstemp failed");

    const char *contents = "MOBI-DATA";
    if (write(fd, contents, 9) != 9) {
      close(fd);
      unlink(path);
      Fail("write failed");
    }
    close(fd);

    std::string out;
    ExpectTrue("path helper reads full file",
               ReadPathToStringLimited(path, &out, 64));
    ExpectEqual("path helper preserves file contents", out, "MOBI-DATA");
    unlink(path);
  }

  return 0;
}
