#include "formats/mobi/mobi_record_scan.h"

#include <algorithm>

namespace mobi_record_scan {

unsigned FirstImageProbeLimit(unsigned remaining_records) {
  return std::min(remaining_records, 128u);
}

unsigned CoverLastResortProbeLimit(unsigned remaining_records) {
  return std::min(remaining_records, 192u);
}

} // namespace mobi_record_scan
