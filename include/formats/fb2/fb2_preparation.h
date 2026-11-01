#pragma once
#include "formats/common/reflow_preparation.h"
class Book;
namespace fb2_parser {
class Preparation {
public:
  Preparation();
  ~Preparation();
  bool Begin(Book *book, const std::string &source);
  ReflowPreparation::Result Step();
  ReflowPreparation::Result GetResult() const;
  void SignalPause();
  void SignalResume();
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;

private:
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
} // namespace fb2_parser
