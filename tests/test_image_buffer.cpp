#include "formats/common/image_buffer.h"
#include <cassert>
#include <limits>
#include <cstdio>
#ifdef IMAGE_TEST_WRAP_MALLOC
static bool fail_large=false;
extern "C" void *__real_malloc(size_t);
extern "C" void *__wrap_malloc(size_t size) {
  return fail_large && size>=1024*1024 ? NULL : __real_malloc(size);
}
#endif

int main() {
  image_memory::Bytes bytes;
  assert(bytes.resize(4));
  bytes.mutable_data()[0]=42;
  assert(!bytes.resize(8u*1024u*1024u+1));
  assert(bytes.size()==4 && bytes[0]==42);
#ifdef IMAGE_TEST_WRAP_MALLOC
  fail_large=true;
  assert(!bytes.resize(1024*1024));
  fail_large=false;
  assert(bytes.size()==4 && bytes[0]==42);
#endif
  assert(!bytes.resize(std::numeric_limits<size_t>::max()));
  const unsigned char original[]={1,2,3};
  bytes.Borrow(original,3);
  assert(bytes.data()==original && !bytes.mutable_data());
  image_memory::Bytes moved(std::move(bytes));
  assert(bytes.empty() && moved.size()==3 && moved[2]==3);
  assert(moved.resize(6));
  assert(moved.mutable_data() && moved[0]==1 && moved[2]==3);
  moved.mutable_data()[0]=8;
  assert(original[0]==1);
  image_memory::Pixels pixels;
  assert(!pixels.resize(std::numeric_limits<size_t>::max()));
  assert(pixels.resize(8));
  pixels.mutable_data()[7]=0x11223344;
  pixels.clear();assert(pixels.empty());
  puts("PASS: checked image buffers, preserved data, borrowing and ownership");
}
