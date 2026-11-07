#pragma once
#include <cstdlib>
#include <new>

// Observe heap requests only around the operation under test; fixtures and
// assertions allocate outside this window.
static bool count_allocations = false;
static size_t allocation_count = 0;
void *operator new(size_t size) {
  if (count_allocations) ++allocation_count;
  void *result = std::malloc(size ? size : 1);
  if (!result) std::abort();
  return result;
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
