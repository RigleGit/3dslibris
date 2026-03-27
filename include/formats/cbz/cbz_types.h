#pragma once

#include <stdint.h>

#include <string>
#include <vector>

struct CbzBitmap {
  int width;
  int height;
  std::vector<uint16_t> pixels;

  CbzBitmap() : width(0), height(0), pixels() {}
};

struct CbzPageEntry {
  std::string path;
  std::string normalized_path;
  unsigned long offset;
  unsigned long uncompressed_size;

  CbzPageEntry() : path(), normalized_path(), offset(0), uncompressed_size(0) {}
};
