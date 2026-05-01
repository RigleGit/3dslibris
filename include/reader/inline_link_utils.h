#pragma once

#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

namespace inline_link_utils {

enum InlineLinkNavDirection {
  INLINE_LINK_NAV_LEFT = 0,
  INLINE_LINK_NAV_RIGHT,
  INLINE_LINK_NAV_UP,
  INLINE_LINK_NAV_DOWN,
};

struct LinkRect {
  int x0;
  int y0;
  int x1;
  int y1;
};

std::string ResolveInternalHref(const std::string &docpath,
                                const std::string &href);
bool IsValidRect(const LinkRect &rect);
int RectCenterX(const LinkRect &rect);
int RectCenterY(const LinkRect &rect);
int FindNeighborIndex(const std::vector<LinkRect> &rects, int current_index,
                      InlineLinkNavDirection direction);
size_t CountInlineLinksInBuffer(const uint32_t *buffer, int length);

} // namespace inline_link_utils
