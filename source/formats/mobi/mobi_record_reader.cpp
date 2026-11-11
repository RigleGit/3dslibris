#include "formats/mobi/mobi_record_reader.h"
#include "formats/common/cache_memory_utils.h"
#include "formats/mobi/mobi_record_scan.h"

#include <algorithm>
#include <new>
#include <unistd.h>

namespace mobi_record_reader {

RecordReader::RecordReader() : fd_(-1), file_size_(0) {}

bool RecordReader::ReadAt(uint64_t offset, void *data, size_t size) const {
  if (fd_ < 0)
    return false;
  if (lseek(fd_, (off_t)offset, SEEK_SET) != (off_t)offset)
    return false;
  unsigned char *p = (unsigned char *)data;
  while (size) {
    const ssize_t n = read(fd_, p, size);
    if (n <= 0)
      return false;
    p += n;
    size -= (size_t)n;
  }
  return true;
}

bool RecordReader::Open(int fd, uint64_t file_size) {
  fd_ = fd;
  file_size_ = file_size;
  offsets_.clear();
  if (fd < 0 || file_size < 78 || file_size > 0xFFFFFFFFull)
    return false;
  unsigned char lead[78];
  if (!ReadAt(0, lead, sizeof(lead)))
    return false;
  const size_t count = mobi_record_scan::ReadBE16(lead + 76);
  const size_t table = 78 + count * 8;
  if (!count || file_size < table)
    return false;
  std::string head;
  if (!cache_memory::HasHeadroom(table, 64 * 1024))
    return false;
  head.resize(table);
  if (!ReadAt(0, &head[0], table))
    return false;
  return mobi_record_scan::ParseRecordOffsets(
      (const unsigned char *)head.data(), head.size(), (size_t)file_size,
      &offsets_);
}

bool RecordReader::Adopt(int fd, uint64_t file_size,
                         const std::vector<uint32_t> &offsets) {
  fd_ = fd;
  file_size_ = file_size;
  offsets_ = offsets;
  return fd >= 0 && offsets_.size() >= 2;
}

size_t RecordReader::RecordSize(uint32_t index) const {
  if ((size_t)index + 1 >= offsets_.size())
    return 0;
  const uint32_t start = offsets_[index], end = offsets_[(size_t)index + 1];
  return end > start && end <= file_size_ ? (size_t)(end - start) : 0;
}

RecordReader::Status RecordReader::Read(uint32_t index,
                                        std::string *out) const {
  if (!out || (size_t)index + 1 >= offsets_.size())
    return BadIndex;
  const size_t size = RecordSize(index);
  if (!size)
    return Empty;
  if (!cache_memory::HasHeadroom(size, 64 * 1024))
    return NoMemory;
  out->resize(size);
  return ReadAt(offsets_[index], &(*out)[0], size) ? Ok : IoError;
}

RecordReader::Status RecordReader::LoadSparse(
    const std::vector<std::pair<uint32_t, uint32_t>> &ranges, size_t max_bytes,
    std::string *compact, std::vector<uint32_t> *offsets) const {
  if (!compact || !offsets || offsets_.size() < 2)
    return BadIndex;
  const size_t count = offsets_.size() - 1;
  std::vector<bool> wanted(count, false);
  size_t total = 0;
  for (size_t r = 0; r < ranges.size(); ++r)
    for (uint64_t i = ranges[r].first; i <= ranges[r].second && i < count; ++i)
      if (!wanted[(size_t)i]) {
        wanted[(size_t)i] = true;
        total += RecordSize((uint32_t)i);
        if (total > max_bytes)
          return NoMemory;
      }
  compact->clear();
  offsets->assign(count + 1, 0);
  if (!cache_memory::HasHeadroom(total, 64 * 1024))
    return NoMemory;
  compact->reserve(total);
  for (size_t i = 0; i < count; ++i) {
    (*offsets)[i] = (uint32_t)compact->size();
    const size_t size = wanted[i] ? RecordSize((uint32_t)i) : 0;
    if (!size)
      continue;
    const size_t at = compact->size();
    compact->resize(at + size);
    if (!ReadAt(offsets_[i], &(*compact)[at], size))
      return IoError;
  }
  (*offsets)[count] = (uint32_t)compact->size();
  return Ok;
}

} // namespace mobi_record_reader
