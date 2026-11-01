#pragma once
#include "formats/common/reflow_preparation.h"
class Book;
namespace mobi_parser {
class Preparation {
public:
  Preparation();
  ~Preparation();
  bool Begin(Book *book, const std::string &source);
  ReflowPreparation::Result Step();
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;
private:
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
}
