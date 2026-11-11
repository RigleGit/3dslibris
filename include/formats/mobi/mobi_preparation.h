#pragma once
#include "formats/common/reflow_preparation.h"
#include "formats/common/reflow_open_stats.h"
#include "formats/common/reflow_open_profile.h"
class Book;
namespace mobi_parser {
struct Hooks;
class Preparation {
public:
  Preparation();
  ~Preparation();
  bool Begin(Book *book, const std::string &source, const Hooks *hooks = NULL);
  ReflowPreparation::Result Step();
  void SetStats(reflow_open_stats::Stats *stats);
  // The profile is owned by the caller and may be downgraded while open.
  void SetProfile(reflow_open_profile::Profile *profile);
  ReflowPreparation::Result GetResult() const;
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;

private:
  ReflowPreparation::Result StepPhase();
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
} // namespace mobi_parser
