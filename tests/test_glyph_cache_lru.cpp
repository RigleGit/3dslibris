#include "ui/glyph_cache_lru.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestNoEvictionBelowCapacity() {
  glyph_cache_lru::GlyphCacheLru lru(2);
  uint32_t evicted = 0;
  ExpectFalse("first insert no eviction", lru.Insert(10, &evicted));
  ExpectFalse("second insert no eviction", lru.Insert(20, &evicted));
  ExpectTrue("contains first", lru.Contains(10));
  ExpectTrue("contains second", lru.Contains(20));
}

void TestEvictsLeastRecentlyUsed() {
  glyph_cache_lru::GlyphCacheLru lru(2);
  uint32_t evicted = 0;
  lru.Insert(10, &evicted);
  lru.Insert(20, &evicted);
  lru.Touch(10);
  ExpectTrue("third insert evicts one", lru.Insert(30, &evicted));
  ExpectEq("evicted lru key", evicted, 20);
  ExpectTrue("contains touched key", lru.Contains(10));
  ExpectTrue("contains new key", lru.Contains(30));
  ExpectFalse("evicted key removed", lru.Contains(20));
}

void TestTouchMissingKeyIgnored() {
  glyph_cache_lru::GlyphCacheLru lru(1);
  uint32_t evicted = 0;
  lru.Touch(99);
  ExpectFalse("insert after missing touch", lru.Insert(10, &evicted));
  ExpectTrue("contains inserted key", lru.Contains(10));
}

} // namespace

int main() {
  TestNoEvictionBelowCapacity();
  TestEvictsLeastRecentlyUsed();
  TestTouchMissingKeyIgnored();
  return 0;
}
