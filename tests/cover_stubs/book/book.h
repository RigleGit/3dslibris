#pragma once
#include <3ds.h>
#include <string>
#include <vector>
#include "shared/status_reporter.h"
class Book {
public:
  struct MuPdfState;
  u16 *coverPixels = nullptr;
  int coverWidth = 0, coverHeight = 0;
  std::string coverImagePath;
  IStatusReporter *GetStatusReporter() const { return nullptr; }
  bool IsOpenAbortRequested() const { return false; }
  ~Book() { delete[] coverPixels; }
};
