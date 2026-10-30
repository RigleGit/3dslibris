#pragma once
#include "formats/common/pdf_view_utils.h"
#include "shared/orientation_utils.h"
#include <algorithm>
#include <cmath>

namespace image_view {
struct Size {
  int width, height;
};
struct Point {float x,y;};
inline Size VirtualSize(int width,int height,int turns) {
  return turns&1 ? Size{height,width} : Size{width,height};
}
inline Point RotatePoint(float x,float y,int turns) {
  switch(turns&3) {
  case 1:return {1-y,x};
  case 2:return {1-x,1-y};
  case 3:return {y,1-x};
  default:return {x,y};
  }
}
inline pdf_view_utils::NormalizedRect RotateRect(
    const pdf_view_utils::NormalizedRect &rect,int turns) {
  const auto a=RotatePoint(rect.left,rect.top,turns);
  const auto b=RotatePoint(rect.left+rect.width,rect.top+rect.height,turns);
  return {std::min(a.x,b.x),std::min(a.y,b.y),std::abs(a.x-b.x),std::abs(a.y-b.y)};
}
inline pdf_view_utils::PreviewLayout RotateLayout(
    const pdf_view_utils::PreviewLayout &r,int width,int height,int turns) {
  switch(turns&3) {
  case 1:return {height-r.y-r.height,r.x,r.height,r.width};
  case 2:return {width-r.x-r.width,height-r.y-r.height,r.width,r.height};
  case 3:return {r.y,width-r.x-r.width,r.height,r.width};
  default:return r;
  }
}
inline Size RequiredSize(int width, int height, int screen_width,
                         int screen_height, int zoom) {
  if (width <= 0 || height <= 0 || screen_width <= 0 || screen_height <= 0)
    return {0, 0};
  const double fit =
      std::min((double)screen_width / width, (double)screen_height / height);
  const double scale = std::min(1.0, fit * pdf_view_utils::ZoomForIndex(zoom));
  return {std::max(1, (int)std::ceil(width * scale)),
          std::max(1, (int)std::ceil(height * scale))};
}
inline void MapPan(unsigned char orientation, float x, float y, float *dx,
                   float *dy) {
  if (orientation_utils::IsLandscape(orientation)) {
    *dx = x;
    *dy = y;
  } else if (orientation_utils::IsTurnedRight(orientation)) {
    *dx = -y;
    *dy = x;
  } else {
    *dx = y;
    *dy = -x;
  }
}
} // namespace image_view
