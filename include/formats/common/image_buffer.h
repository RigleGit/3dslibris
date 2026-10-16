#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <stdint.h>

namespace image_memory {
// Large image payloads must report allocation failure on exception-free builds.
// Borrowed resources remain owned by the open Book and are used synchronously.
template <typename T> class Buffer {
  struct Free { void operator()(T *p) const { std::free(p); } };
  std::unique_ptr<T,Free> owned_;
  const T *data_=NULL;
  size_t size_=0,capacity_=0;
public:
  Buffer() {}
  Buffer(Buffer &&other) { swap(other); }
  Buffer &operator=(Buffer &&other) {
    Buffer next(std::move(other));swap(next);return *this;
  }
  const T *data() const { return data_; }
  T *mutable_data() { return owned_.get(); }
  size_t size() const { return size_; }
  bool empty() const { return size_==0; }
  const T &operator[](size_t i) const { return data_[i]; }
  bool resize(size_t count,size_t max_bytes=8u*1024u*1024u) {
    if(count>max_bytes/sizeof(T)) return false;
    if(!count) {clear();return true;}
    if(owned_ && count<=capacity_) {size_=count;return true;}
    T *next=static_cast<T*>(std::malloc(count*sizeof(T)));
    if(!next) return false;
    if(size_) std::memcpy(next,data_,std::min(count,size_)*sizeof(T));
    owned_.reset(next);data_=next;capacity_=size_=count;return true;
  }
  void clear() { owned_.reset();data_=NULL;size_=capacity_=0; }
  void Borrow(const T *data,size_t count) {
    clear();data_=data;size_=data ? count : 0;
  }
  void swap(Buffer &other) {
    owned_.swap(other.owned_);std::swap(data_,other.data_);
    std::swap(size_,other.size_);std::swap(capacity_,other.capacity_);
  }
  void RemovePrefix(size_t count) {
    count=std::min(count,size_);
    if(owned_) std::memmove(owned_.get(),data_+count,(size_-count)*sizeof(T));
    else if(data_) data_+=count;
    size_-=count;
  }
};
typedef Buffer<unsigned char> Bytes;
typedef Buffer<uint32_t> Pixels;
} // namespace image_memory
