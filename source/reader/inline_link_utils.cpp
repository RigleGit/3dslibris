#include "reader/inline_link_utils.h"

#include "path_utils.h"
#include "shared/text_token_constants.h"

#include <limits>

namespace inline_link_utils {

namespace {

int AxisDelta(const LinkRect &from, const LinkRect &to,
              InlineLinkNavDirection direction) {
  const int from_x = RectCenterX(from);
  const int from_y = RectCenterY(from);
  const int to_x = RectCenterX(to);
  const int to_y = RectCenterY(to);
  switch (direction) {
  case INLINE_LINK_NAV_LEFT:
    return from_x - to_x;
  case INLINE_LINK_NAV_RIGHT:
    return to_x - from_x;
  case INLINE_LINK_NAV_UP:
    return from_y - to_y;
  case INLINE_LINK_NAV_DOWN:
    return to_y - from_y;
  }
  return -1;
}

int CrossDelta(const LinkRect &from, const LinkRect &to,
               InlineLinkNavDirection direction) {
  const int from_x = RectCenterX(from);
  const int from_y = RectCenterY(from);
  const int to_x = RectCenterX(to);
  const int to_y = RectCenterY(to);
  switch (direction) {
  case INLINE_LINK_NAV_LEFT:
  case INLINE_LINK_NAV_RIGHT:
    return to_y - from_y;
  case INLINE_LINK_NAV_UP:
  case INLINE_LINK_NAV_DOWN:
    return to_x - from_x;
  }
  return 0;
}

} // namespace

std::string ResolveInternalHref(const std::string &docpath,
                                const std::string &href_raw) {
  const std::string href = UrlDecode(href_raw);
  if (href.empty())
    return "";
  if (href.find("://") != std::string::npos)
    return "";
  if (href.compare(0, 5, "data:") == 0)
    return "";

  if (href[0] == '#') {
    if (docpath.empty() || href.size() <= 1)
      return "";
    return NormalizePath(docpath) + href;
  }

  const size_t hash = href.find('#');
  const std::string path_part =
      (hash == std::string::npos) ? href : href.substr(0, hash);
  const std::string fragment =
      (hash == std::string::npos) ? "" : href.substr(hash);
  if (path_part.empty()) {
    if (docpath.empty() || fragment.empty())
      return "";
    return NormalizePath(docpath) + fragment;
  }

  const std::string resolved = ResolveRelativePath(docpath, path_part);
  if (resolved.empty())
    return "";
  return fragment.empty() ? resolved : (resolved + fragment);
}

bool IsValidRect(const LinkRect &rect) {
  return rect.x1 > rect.x0 && rect.y1 > rect.y0;
}

int RectCenterX(const LinkRect &rect) { return rect.x0 + ((rect.x1 - rect.x0) / 2); }

int RectCenterY(const LinkRect &rect) { return rect.y0 + ((rect.y1 - rect.y0) / 2); }

int FindNeighborIndex(const std::vector<LinkRect> &rects, int current_index,
                      InlineLinkNavDirection direction) {
  if (current_index < 0 || current_index >= (int)rects.size())
    return -1;
  if (!IsValidRect(rects[(size_t)current_index]))
    return -1;

  const LinkRect &current = rects[(size_t)current_index];
  int best_index = -1;
  long best_score = std::numeric_limits<long>::max();

  for (size_t i = 0; i < rects.size(); ++i) {
    if ((int)i == current_index || !IsValidRect(rects[i]))
      continue;
    const int axis = AxisDelta(current, rects[i], direction);
    if (axis <= 0)
      continue;
    const int cross = CrossDelta(current, rects[i], direction);
    const long score = (long)axis * 1000L + (long)cross * (long)cross;
    if (score < best_score) {
      best_score = score;
      best_index = (int)i;
    }
  }

  return best_index;
}

size_t CountInlineLinksInBuffer(const uint32_t *buffer, int length) {
  if (!buffer || length <= 1)
    return 0;

  size_t count = 0;
  for (int i = 0; i + 1 < length; ++i) {
    if (buffer[i] == TEXT_LINK_START && buffer[i + 1] != 0) {
      count++;
      i++;
    }
  }
  return count;
}

} // namespace inline_link_utils
