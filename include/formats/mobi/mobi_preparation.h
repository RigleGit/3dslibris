#pragma once
#include "formats/common/reflow_preparation.h"
class Book;
namespace mobi_parser {
struct Hooks;
class Preparation {
public:
  Preparation();
  ~Preparation();
  bool Begin(Book *book, const std::string &source, const Hooks *hooks = NULL);
  ReflowPreparation::Result Step();
  ReflowPreparation::Result GetResult() const;
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;

private:
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
} // namespace mobi_parser
