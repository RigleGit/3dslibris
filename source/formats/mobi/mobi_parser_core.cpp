/*
    3dslibris - mobi_parser_core.cpp
*/

#include "formats/mobi/mobi_parser_core.h"

#include "formats/common/book_error.h"
#include "formats/common/file_read_utils.h"
#include "formats/mobi/mobi_record_decode.h"
#include "formats/mobi/mobi_record_scan.h"
#include "formats/mobi/mobi_structured_toc_parser.h"

#include <3ds.h>

namespace mobi_parser_core {

MobiHeaderInfo::MobiHeaderInfo()
    : compression(0), text_len(0), text_rec_count(0), encoding(1252),
      first_non_book_index(0), mobi_full_name_off(0), mobi_full_name_len(0),
      huffcdic_record_index(0), num_huffcdic_records(0), trailing_flags(0),
      ncx_index(mobi_structured_toc_parser::kMobiNullIndex) {}

u8 LoadMobiSource(const char *path, std::string *raw, u64 *t_after_read,
                  size_t max_bytes) {
  if (!path || !raw)
    return BOOK_ERR_CORRUPT;
  raw->clear();
  if (!file_read_utils::ReadPathToStringLimited(path, raw, max_bytes))
    return 252;
  if (raw->empty())
    return BOOK_ERR_CORRUPT;
  if (t_after_read)
    *t_after_read = osGetTime();
  return 0;
}

u8 ParseMobiHeader(const std::string &raw, MobiHeaderInfo *header) {
  if (!header)
    return 254;

  header->offsets.clear();
  if (!mobi_record_scan::ParseRecordOffsets(raw, &header->offsets) ||
      header->offsets.size() < 3)
    return BOOK_ERR_CORRUPT;

  const u8 *data = (const u8 *)raw.data();
  const u32 rec0_start = header->offsets[0];
  const u32 rec0_end = header->offsets[1];
  if (rec0_end <= rec0_start || rec0_end - rec0_start < 16)
    return 254;

  const u8 *rec0 = data + rec0_start;
  const size_t rec0_len = (size_t)(rec0_end - rec0_start);
  mobi_record_decode::MobiRecord0Header rec0_header;
  if (!mobi_record_decode::ParseRecord0Header(rec0, rec0_len, &rec0_header))
    return 254;
  header->compression = rec0_header.compression;
  header->text_len = rec0_header.text_len;
  header->text_rec_count = rec0_header.text_rec_count;
  header->encoding = rec0_header.encoding;
  header->first_non_book_index = rec0_header.resource_start;
  header->mobi_full_name_off = rec0_header.title_offset;
  header->mobi_full_name_len = rec0_header.title_length;
  header->huffcdic_record_index = rec0_header.huffcdic_record_index;
  header->num_huffcdic_records = rec0_header.num_huffcdic_records;
  header->trailing_flags = rec0_header.trailing_flags;
  header->ncx_index = rec0_header.indx_index;

  u32 max_text_records = (u32)header->offsets.size() - 2;
  if (header->text_rec_count == 0 || header->text_rec_count > max_text_records)
    header->text_rec_count = max_text_records;
  if (header->first_non_book_index > 1) {
    u32 boundary = header->first_non_book_index - 1;
    if (boundary > 0 && boundary < header->text_rec_count)
      header->text_rec_count = boundary;
  }
  if (header->text_rec_count == 0)
    return 255;

  return 0;
}

bool BuildMobiMergedText(const std::string &raw, const MobiHeaderInfo &header,
                         std::string *merged) {
  if (!merged)
    return false;
  mobi_record_decode::MobiRecord0Header rec0;
  rec0.compression = header.compression;
  rec0.text_len = header.text_len;
  rec0.text_rec_count = header.text_rec_count;
  rec0.encoding = header.encoding;
  rec0.resource_start = header.first_non_book_index;
  rec0.title_offset = header.mobi_full_name_off;
  rec0.title_length = header.mobi_full_name_len;
  rec0.huffcdic_record_index = header.huffcdic_record_index;
  rec0.num_huffcdic_records = header.num_huffcdic_records;
  rec0.trailing_flags = header.trailing_flags;
  rec0.indx_index = header.ncx_index;
  return mobi_record_decode::BuildMergedText(raw, header.offsets, rec0, merged);
}

bool IsSupportedCompression(u16 compression) {
  return compression == 1 || compression == 2 || compression == 17480;
}

} // namespace mobi_parser_core
