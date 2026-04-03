/*
    3dslibris - mobi_parser_core.h

    Core MOBI parser helpers shared by format dispatch.
*/

#pragma once

#include <3ds/types.h>

#include <string>
#include <vector>

namespace mobi_parser_core {

struct MobiHeaderInfo {
  u16 compression;
  u32 text_len;
  u32 text_rec_count;
  u32 encoding;
  u32 first_non_book_index;
  u32 mobi_full_name_off;
  u32 mobi_full_name_len;
  u32 huffcdic_record_index;
  u32 num_huffcdic_records;
  u32 trailing_flags;
  u32 ncx_index;
  std::vector<u32> offsets;

  MobiHeaderInfo();
};

u8 LoadMobiSource(const char *path, std::string *raw, u64 *t_after_read,
                  size_t max_bytes);

u8 ParseMobiHeader(const std::string &raw, MobiHeaderInfo *header);

bool BuildMobiMergedText(const std::string &raw, const MobiHeaderInfo &header,
                         std::string *merged);

bool IsSupportedCompression(u16 compression);

} // namespace mobi_parser_core
