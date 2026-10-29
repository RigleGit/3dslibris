#pragma once
#include <3ds.h>
class Book {
public:
  u16 *coverPixels = nullptr;
  int coverWidth = 0, coverHeight = 0;
  ~Book() { delete[] coverPixels; }
};
