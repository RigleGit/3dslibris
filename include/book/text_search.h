#pragma once

#include "book/page_search_data.h"
#include <memory>
#include <string>

namespace text_search {
struct Position {
  uint32_t page, token;
  Position(uint32_t p = 0, uint32_t t = 0) : page(p), token(t) {}
};
inline bool operator<(Position a, Position b) {
  return a.page < b.page || (a.page == b.page && a.token < b.token);
}
inline bool operator==(Position a, Position b) {
  return a.page == b.page && a.token == b.token;
}
struct Match { Position start, end; }; // end is exclusive
struct PageView {
  const uint32_t *text;
  size_t length;
  const page_search::Events *events;
};
enum PageAvailability { PageReady, PagePending, PageError };
struct Source {
  void *context;
  uint32_t pages;
  bool (*page)(void *, uint32_t, PageView *);
  PageAvailability (*prepare)(void *, uint32_t);
  Source(void *ctx = nullptr, uint32_t count = 0,
         bool (*read)(void *, uint32_t, PageView *) = nullptr,
         PageAvailability (*ensure)(void *, uint32_t) = nullptr)
      : context(ctx), pages(count), page(read), prepare(ensure) {}
};
enum Status { Idle, Searching, Found, NotFound, Failed };
// No platform calls or retained Page pointers. The owner invalidates the session
// before replacing pages. Pump bounds work even within a single large page.
class Engine {
public:
  Engine();
  ~Engine();
  bool Configure(const std::string &query, bool whole_words);
  void Start(const Source &source, Position anchor, bool backwards,
             bool include_anchor = false);
  Status Pump(unsigned steps);
  void Cancel();
  Status GetStatus() const;
  bool IsWaitingForPage() const;
  const Match &GetMatch() const;
  const std::string &GetError() const;
  bool Wrapped() const;
  bool ExcludedRtl() const;
  uint64_t Scanned() const;
  size_t AuxiliaryBytes() const;
private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace text_search
