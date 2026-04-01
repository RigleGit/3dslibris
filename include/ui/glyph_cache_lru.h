#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

namespace glyph_cache_lru {

class GlyphCacheLru {
public:
  explicit GlyphCacheLru(size_t capacity) : capacity_(capacity) {}

  void Clear() {
    order_.clear();
    positions_.clear();
  }

  bool Contains(uint32_t key) const {
    return positions_.find(key) != positions_.end();
  }

  void Touch(uint32_t key) {
    auto it = positions_.find(key);
    if (it == positions_.end())
      return;
    order_.splice(order_.end(), order_, it->second);
    it->second = --order_.end();
  }

  bool Insert(uint32_t key, uint32_t *evicted_key) {
    if (evicted_key)
      *evicted_key = 0;

    if (capacity_ == 0)
      return false;

    auto existing = positions_.find(key);
    if (existing != positions_.end()) {
      Touch(key);
      return false;
    }

    bool evicted = false;
    if (order_.size() >= capacity_) {
      uint32_t lru_key = order_.front();
      order_.pop_front();
      positions_.erase(lru_key);
      if (evicted_key)
        *evicted_key = lru_key;
      evicted = true;
    }

    order_.push_back(key);
    positions_[key] = --order_.end();
    return evicted;
  }

private:
  size_t capacity_;
  std::list<uint32_t> order_;
  std::unordered_map<uint32_t, std::list<uint32_t>::iterator> positions_;
};

} // namespace glyph_cache_lru
