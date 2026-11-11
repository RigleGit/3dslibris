#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mobi_record_scan {

std::uint16_t ReadBE16(const unsigned char *p);
std::uint32_t ReadBE32(const unsigned char *p);
bool ParseRecordOffsets(const std::string &raw, std::vector<std::uint32_t> *offsets);
// Same parse over the leading `avail` bytes of a file of `total_size` bytes
// (PDB header plus record table are enough).
bool ParseRecordOffsets(const unsigned char *buf, std::size_t avail,
                        std::size_t total_size,
                        std::vector<std::uint32_t> *offsets);

unsigned FirstImageProbeLimit(unsigned remaining_records);
unsigned CoverLastResortProbeLimit(unsigned remaining_records);

} // namespace mobi_record_scan
