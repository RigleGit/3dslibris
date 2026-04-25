#include "reader/inline_link_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected \"" + expected + "\", got \"" +
         actual + "\"");
  }
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void TestResolveInternalHref() {
  using namespace inline_link_utils;

  ExpectEq("same doc fragment", ResolveInternalHref("OPS/ch1.xhtml", "#part-2"),
           "OPS/ch1.xhtml#part-2");
  ExpectEq("relative doc fragment",
           ResolveInternalHref("OPS/ch1.xhtml", "sub/ch2.xhtml#frag"),
           "OPS/sub/ch2.xhtml#frag");
  ExpectEq("relative doc no fragment",
           ResolveInternalHref("OPS/ch1.xhtml", "../nav.xhtml"),
           "nav.xhtml");
  ExpectEq("empty href rejected", ResolveInternalHref("OPS/ch1.xhtml", ""), "");
  ExpectEq("external href rejected",
           ResolveInternalHref("OPS/ch1.xhtml", "https://example.com"), "");
  ExpectEq("data href rejected",
           ResolveInternalHref("OPS/ch1.xhtml", "data:text/plain,hi"), "");
}

void TestLinkNavigation() {
  using namespace inline_link_utils;

  std::vector<LinkRect> rects;
  rects.push_back({10, 10, 40, 20});
  rects.push_back({60, 10, 90, 20});
  rects.push_back({10, 30, 40, 40});
  rects.push_back({60, 30, 90, 40});

  ExpectEq("right from top-left",
           FindNeighborIndex(rects, 0, INLINE_LINK_NAV_RIGHT), 1);
  ExpectEq("left from top-right",
           FindNeighborIndex(rects, 1, INLINE_LINK_NAV_LEFT), 0);
  ExpectEq("down from top-left",
           FindNeighborIndex(rects, 0, INLINE_LINK_NAV_DOWN), 2);
  ExpectEq("up from bottom-right",
           FindNeighborIndex(rects, 3, INLINE_LINK_NAV_UP), 1);
  ExpectEq("no left neighbor on edge",
           FindNeighborIndex(rects, 0, INLINE_LINK_NAV_LEFT), -1);
}

void TestLinkRectHelpers() {
  using namespace inline_link_utils;

  LinkRect rect = {5, 7, 15, 19};
  ExpectEq("center x", RectCenterX(rect), 10);
  ExpectEq("center y", RectCenterY(rect), 13);
  ExpectTrue("rect valid", IsValidRect(rect));
  ExpectFalse("rect invalid", IsValidRect({5, 5, 5, 10}));
}

} // namespace

int main() {
  TestResolveInternalHref();
  TestLinkNavigation();
  TestLinkRectHelpers();
  return 0;
}
