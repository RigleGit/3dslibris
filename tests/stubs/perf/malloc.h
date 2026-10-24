#pragma once

extern int fake_heap_used;
extern int fake_heap_free;
struct mallinfo {
  int uordblks;
  int fordblks;
};
inline struct mallinfo mallinfo() {
  return {fake_heap_used, fake_heap_free};
}
