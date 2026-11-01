#pragma once
#include <cstddef>
#include <string>
class Book;
namespace reflow_block_cache {
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
class Writer {
public:
  Writer();
  ~Writer();
  bool Begin(Book *book, const std::string &path);
  bool AppendDocument(unsigned first, unsigned end);
  Result Step();
  bool Finish();
  void Abort();
  Result GetResult() const;
  unsigned Progress() const;

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
               bool validate_only = false);
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

private:
  Result Advance();
  struct State;
  State *state_;
  Reader(const Reader &);
  Reader &operator=(const Reader &);
};
} // namespace reflow_block_cache
