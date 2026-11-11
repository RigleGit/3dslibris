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
  // Prefix of the temporary files of the stream tier (default: the source path).
  void SetScratchBase(const std::string &base);
  ReflowPreparation::Result GetResult() const;
  const char *Label() const;
  unsigned Progress() const;
  unsigned Total() const;
  // Stream tier only: pages the writer has finished with may give their token
  // buffers back, so memory does not follow the length of the book. The
  // structured TOC still checks headings on nearby pages, so the lines of the
  // pages it will look at are captured first. `keep` stays resident.
  // ReleasableEnd returns the first page index that may NOT be released yet
  // (== released() when nothing can be).
  unsigned ReleasableEnd(unsigned written) const;
  // True when at least one page buffer was actually freed.
  bool ReleasePages(unsigned end, int keep);
  // Uncompressed text length from record 0; 0 until the header was parsed.
  unsigned long long TextLength() const;

private:
  ReflowPreparation::Result StepPhase();
  ReflowPreparation::Result StepStream();
  ReflowPreparation::Result StepToc();
  struct State;
  State *state_;
  Preparation(const Preparation &);
  Preparation &operator=(const Preparation &);
};
} // namespace mobi_parser
