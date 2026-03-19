#include "formats/mobi/mobi_heading_markers.h"

namespace {

static const unsigned char kHeadingMarkerH1 = 15;
static const unsigned char kHeadingMarkerH2 = 16;
static const unsigned char kHeadingMarkerH3 = 17;

} // namespace

namespace mobi_heading_markers {

unsigned char MarkerForHeadingLevel(int heading_level) {
  if (heading_level <= 1)
    return kHeadingMarkerH1;
  if (heading_level == 2)
    return kHeadingMarkerH2;
  if (heading_level == 3)
    return kHeadingMarkerH3;
  return 0;
}

int HeadingLevelFromMarker(unsigned char marker) {
  if (marker == kHeadingMarkerH1)
    return 1;
  if (marker == kHeadingMarkerH2)
    return 2;
  if (marker == kHeadingMarkerH3)
    return 3;
  return 0;
}

} // namespace mobi_heading_markers
