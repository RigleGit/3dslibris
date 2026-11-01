#pragma once
#include <cstddef>
#include <string>
class Book;
namespace epub_parser {
class Preparation {
public:
  enum Result { Pending, Complete, Cancelled, Failed };
  Preparation();
  ~Preparation();
  bool Begin(Book *book, const std::string &source,
             size_t residency_budget = 0);
  Result Step();
  void Cancel();
  Result GetResult() const;
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;
  bool PersistenceFailed() const;

private:
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
} // namespace epub_parser
