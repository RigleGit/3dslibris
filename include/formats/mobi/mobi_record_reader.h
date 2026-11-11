#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace mobi_record_reader {

// File-backed view of a PDB/MOBI record table. Records are read on demand with
// lseek + read, so only the record being used is resident. The descriptor is
// borrowed: the caller keeps it open for the lifetime of the reader.
class RecordReader {
public:
  enum Status {
    Ok,       // *out holds the record bytes
    Empty,    // zero-length record (same cases GetRecordSlice rejects)
    BadIndex, // no such record
    IoError,
    NoMemory
  };

  RecordReader();
  // Reads the PDB header and record table (same validation as
  // mobi_record_scan::ParseRecordOffsets on the whole file).
  bool Open(int fd, uint64_t file_size);
  // Reuses a record table parsed from the same file.
  bool Adopt(int fd, uint64_t file_size, const std::vector<uint32_t> &offsets);

  const std::vector<uint32_t> &Offsets() const { return offsets_; }
  uint64_t FileSize() const { return file_size_; }
  size_t RecordCount() const {
    return offsets_.empty() ? 0 : offsets_.size() - 1;
  }
  size_t RecordSize(uint32_t index) const;
  Status Read(uint32_t index, std::string *out) const;
  // Records are served from a read-ahead block (text records sit next to each
  // other, so one SD read covers many). 0 disables it; the default is 192 KiB.
  void SetReadAhead(size_t bytes);
  // Reads that reached the file (not served from the block).
  unsigned DiskReads() const { return disk_reads_; }

  // Loads the records of every inclusive [first, last] range into one compact
  // buffer. offsets keeps the whole table's shape (record_count + 1 entries):
  // loaded records get their bytes, all others are empty, so slice helpers that
  // take (raw, offsets) see identical data for the loaded records. Fails when
  // the loaded bytes would exceed max_bytes.
  Status LoadSparse(const std::vector<std::pair<uint32_t, uint32_t>> &ranges,
                    size_t max_bytes, std::string *compact,
                    std::vector<uint32_t> *offsets) const;

private:
  bool ReadAt(uint64_t offset, void *data, size_t size) const;
  int fd_;
  uint64_t file_size_;
  std::vector<uint32_t> offsets_;
  mutable std::string block_;
  mutable uint64_t block_start_;
  mutable size_t block_len_;
  size_t read_ahead_;
  mutable unsigned disk_reads_;
};

} // namespace mobi_record_reader
