#include "book/page_buffer_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestReusesExistingCapacity() {
  ExpectEq("reuse equal capacity",
           page_buffer_utils::RequiredPageBufferCapacity(128, 128),
           (size_t)128);
  ExpectEq("reuse larger capacity",
           page_buffer_utils::RequiredPageBufferCapacity(256, 128),
           (size_t)256);
}

void TestGrowsWhenNeeded() {
  ExpectEq("grow empty buffer",
           page_buffer_utils::RequiredPageBufferCapacity(0, 64), (size_t)64);
  ExpectEq("grow undersized buffer",
           page_buffer_utils::RequiredPageBufferCapacity(63, 64), (size_t)64);
}

void TestEmptyPayloadNeedsNoCapacity() {
  ExpectEq("zero length stays zero",
           page_buffer_utils::RequiredPageBufferCapacity(512, 0), (size_t)0);
}

void TestPageVectorReserveCapacity() {
  ExpectEq("reserve grows by incoming count",
           page_buffer_utils::RequiredPageVectorCapacity(2, 2, 3), (size_t)5);
  ExpectEq("reserve keeps larger capacity",
           page_buffer_utils::RequiredPageVectorCapacity(2, 8, 3), (size_t)8);
  ExpectEq("reserve no-op with no incoming pages",
           page_buffer_utils::RequiredPageVectorCapacity(2, 8, 0), (size_t)8);
}

void TestAdoptPageBufferMove() {
  std::vector<unsigned char> src;
  src.push_back(1);
  src.push_back(2);
  src.push_back(3);

  page_buffer_utils::OwnedPageBuffer adopted =
      page_buffer_utils::AdoptPageBuffer(&src);
  ExpectEq("adopted size", adopted.bytes.size(), (size_t)3);
  ExpectEq("source drained", src.size(), (size_t)0);
}

} // namespace

int main() {
  TestReusesExistingCapacity();
  TestGrowsWhenNeeded();
  TestEmptyPayloadNeedsNoCapacity();
  TestPageVectorReserveCapacity();
  TestAdoptPageBufferMove();
  return 0;
}
