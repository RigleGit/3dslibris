#pragma once

static const int kTextPixelSizeMin = 8;
static const int kTextPixelSizeMax = 20;

inline int ClampTextPixelSize(int size) {
  if (size < kTextPixelSizeMin)
    return kTextPixelSizeMin;
  if (size > kTextPixelSizeMax)
    return kTextPixelSizeMax;
  return size;
}
