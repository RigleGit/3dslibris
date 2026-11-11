#pragma once
#include <cstddef>
#include <string>
#include "formats/common/image_buffer.h"
class Book;
namespace reflow_block_cache {
enum Format { Epub, Mobi, Fb2 };
enum Result : int {
  Pending,
  Ready,
  Complete,
  Missing,
  Cancelled,
  Corrupt,
  IoError,
  NoMemory
};
struct WriterStats {
  unsigned blocks = 0, documents = 0, write_calls = 0;
  unsigned long long packed_bytes = 0, max_block = 0, encode_us = 0,
                     deflate_us = 0, crc_us = 0, io_ms = 0;
};
class Writer {
public:
  Writer();
  ~Writer();
  bool Begin(Book *book, const std::string &path, Format format = Epub);
  bool AppendDocument(unsigned first, unsigned end);
  // budget_ms bounds one cooperative slice (one physical write may overrun).
  Result Step(unsigned budget_ms = 50);
  bool Finish();
  void Abort();
  Result GetResult() const;
  unsigned Progress() const;
  WriterStats GetStats() const;

private:
  struct State;
  State *state_;
  Writer(const Writer &);
  Writer &operator=(const Writer &);
};
class Reader {
public:
  Reader();
  ~Reader();
  // A zero budget requests the complete cooperative reader. Nonzero enables
  // residency.
  Result Begin(Book *book, const std::string &path, size_t budget,
               bool validate_only = false, Format format = Epub);
  Result Step();
  Result GetResult() const;
  void Close();
  bool ActivatePreparedPages();
  bool Request(unsigned page);
  bool IsReady(unsigned page) const;
  unsigned Links(unsigned page) const;
  void Pin(unsigned page);
  void InvalidateFile();
  void FrameEnd();
  size_t ResidentBytes() const;
  size_t BytesRead() const;
  bool HasIndex() const;
  // Resource errors never change the pending page result.
  Result ReadResource(const std::string &id, image_memory::Bytes *out);

private:
  Result Advance();
  struct State;
  State *state_;
  Reader(const Reader &);
  Reader &operator=(const Reader &);
};
} // namespace reflow_block_cache
