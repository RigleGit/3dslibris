/*
    3dslibris - mobi_structured_toc_parser.cpp

    Structured TOC parser for MOBI INDX/TAGX/CNCX records.
*/

#include "formats/mobi/mobi_structured_toc_parser.h"

#include <string.h>

#include <unordered_map>

#include "shared/debug_log.h"
#include "formats/mobi/mobi_record_scan.h"
#include "shared/status_reporter.h"

namespace {

struct MobiIndxHeader {
  u32 start;
  u32 count;
  u32 ncncx;
  u32 tagx;
  size_t header_end;
};

struct MobiTagXRule {
  u8 tag;
  u8 num_values;
  u8 bitmask;
  u8 eof;
};

struct MobiPendingTag {
  u8 tag;
  u8 num_values;
  u8 value_count;
  bool has_value_bytes;
  u32 value_bytes;
};

typedef std::unordered_map<u8, std::vector<u32>> MobiTagMap;

int CountSetBits8(u8 v) {
  int count = 0;
  while (v) {
    count += (v & 1) ? 1 : 0;
    v >>= 1;
  }
  return count;
}

bool ReadMobiVwiForward(const u8 *data, size_t len, size_t *consumed,
                        u32 *value) {
  if (!data || !consumed || !value || len == 0)
    return false;
  *consumed = 0;
  *value = 0;
  for (size_t i = 0; i < len; i++) {
    u8 b = data[i];
    *value = (*value << 7) | (u32)(b & 0x7F);
    *consumed = i + 1;
    if (b & 0x80)
      return true;
  }
  return false;
}

size_t FindMobiSignature(const u8 *data, size_t len, const char *sig,
                         size_t sig_len, size_t start) {
  if (!data || !sig || sig_len == 0 || start >= len)
    return SIZE_MAX;
  for (size_t i = start; i + sig_len <= len; i++) {
    if (memcmp(data + i, sig, sig_len) == 0)
      return i;
  }
  return SIZE_MAX;
}

bool GetMobiRecordSlice(const std::string &raw, const std::vector<u32> &offsets,
                        u32 rec_idx, const u8 **data_out, size_t *len_out) {
  if (!data_out || !len_out || rec_idx + 1 >= offsets.size())
    return false;
  const u32 start = offsets[(size_t)rec_idx];
  const u32 end = offsets[(size_t)rec_idx + 1];
  if (end <= start || end > raw.size())
    return false;
  *data_out = (const u8 *)raw.data() + start;
  *len_out = (size_t)(end - start);
  return true;
}

bool ParseMobiIndxHeader(const u8 *data, size_t len, MobiIndxHeader *out) {
  if (!data || !out || len < 184)
    return false;
  if (memcmp(data, "INDX", 4) != 0)
    return false;
  out->start = (u32)mobi_record_scan::ReadBE32(data + 20);
  out->count = (u32)mobi_record_scan::ReadBE32(data + 24);
  out->ncncx = (u32)mobi_record_scan::ReadBE32(data + 52);
  out->tagx = (u32)mobi_record_scan::ReadBE32(data + 180);
  out->header_end = 184;
  return true;
}

bool ParseMobiTagxSection(const u8 *data, size_t len, size_t tagx_start,
                          u32 *control_bytes,
                          std::vector<MobiTagXRule> *rules) {
  if (!data || !control_bytes || !rules || tagx_start >= len)
    return false;
  rules->clear();
  *control_bytes = 0;

  if (tagx_start + 12 > len || memcmp(data + tagx_start, "TAGX", 4) != 0)
    return false;

  const u32 first_entry_off =
      (u32)mobi_record_scan::ReadBE32(data + tagx_start + 4);
  *control_bytes = (u32)mobi_record_scan::ReadBE32(data + tagx_start + 8);
  if (first_entry_off < 12 || tagx_start + first_entry_off > len)
    return false;

  for (size_t i = tagx_start + 12; i + 4 <= tagx_start + first_entry_off;
       i += 4) {
    MobiTagXRule r;
    r.tag = data[i + 0];
    r.num_values = data[i + 1];
    r.bitmask = data[i + 2];
    r.eof = data[i + 3];
    rules->push_back(r);
  }
  return !rules->empty();
}

bool ParseMobiTagMap(const u8 *data, size_t len, u32 control_bytes,
                     const std::vector<MobiTagXRule> &rules, MobiTagMap *out) {
  if (!data || !out)
    return false;
  out->clear();
  if (len == 0 || control_bytes > len)
    return false;

  const u8 *control = data;
  size_t cursor = (size_t)control_bytes;
  size_t control_index = 0;
  std::vector<MobiPendingTag> pending;
  pending.reserve(rules.size());

  for (size_t i = 0; i < rules.size(); i++) {
    const MobiTagXRule &rule = rules[i];
    if (rule.eof == 0x01) {
      control_index++;
      continue;
    }
    if (control_index >= control_bytes)
      break;

    u8 value = (u8)(control[control_index] & rule.bitmask);
    if (value == 0)
      continue;

    MobiPendingTag p;
    p.tag = rule.tag;
    p.num_values = (rule.num_values == 0) ? 1 : rule.num_values;
    p.value_count = 0;
    p.has_value_bytes = false;
    p.value_bytes = 0;

    if (value == rule.bitmask) {
      if (CountSetBits8(rule.bitmask) > 1) {
        size_t consumed = 0;
        u32 value_bytes = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed,
                                &value_bytes))
          return false;
        cursor += consumed;
        p.has_value_bytes = true;
        p.value_bytes = value_bytes;
      } else {
        p.value_count = 1;
      }
    } else {
      u8 mask = rule.bitmask;
      while ((mask & 0x01) == 0) {
        mask >>= 1;
        value >>= 1;
      }
      p.value_count = value;
    }
    pending.push_back(p);
  }

  for (size_t i = 0; i < pending.size(); i++) {
    const MobiPendingTag &p = pending[i];
    std::vector<u32> values;

    if (!p.has_value_bytes) {
      const size_t nvalues = (size_t)p.value_count * (size_t)p.num_values;
      for (size_t j = 0; j < nvalues; j++) {
        if (cursor >= len)
          return false;
        size_t consumed = 0;
        u32 v = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed, &v))
          return false;
        cursor += consumed;
        values.push_back(v);
      }
    } else {
      size_t consumed_total = 0;
      while (consumed_total < p.value_bytes) {
        if (cursor >= len)
          return false;
        size_t consumed = 0;
        u32 v = 0;
        if (!ReadMobiVwiForward(data + cursor, len - cursor, &consumed, &v))
          return false;
        cursor += consumed;
        consumed_total += consumed;
        values.push_back(v);
      }
      if (consumed_total != p.value_bytes)
        return false;
    }
    (*out)[p.tag] = values;
  }

  return true;
}

void ParseMobiCncxRecords(
    const std::string &raw, const std::vector<u32> &offsets, u32 first_record,
    u32 record_count, u32 encoding,
    std::unordered_map<u32, std::string> *cncx_text,
    const mobi_structured_toc_parser::ParseCallbacks &callbacks) {
  if (!cncx_text)
    return;
  cncx_text->clear();
  for (u32 r = 0; r < record_count; r++) {
    const u32 rec_idx = first_record + r;
    const u8 *rec = NULL;
    size_t rec_len = 0;
    if (!GetMobiRecordSlice(raw, offsets, rec_idx, &rec, &rec_len))
      break;

    size_t pos = 0;
    while (pos < rec_len) {
      if (rec[pos] == 0)
        break;
      size_t consumed = 0;
      u32 slen = 0;
      if (!ReadMobiVwiForward(rec + pos, rec_len - pos, &consumed, &slen))
        break;

      const size_t text_start = pos + consumed;
      if (text_start + (size_t)slen > rec_len)
        break;

      std::string raw_txt((const char *)rec + text_start, (size_t)slen);
      std::string txt = callbacks.decode_bytes_to_utf8(raw_txt, encoding);
      txt = callbacks.normalize_title(txt);
      if (!txt.empty()) {
        const u32 key = (u32)pos + (r * 0x10000u);
        (*cncx_text)[key] = txt;
      }
      pos = text_start + (size_t)slen;
    }
  }
}

std::string DecodeMobiIndexIdent(
    const u8 *entry, size_t len, u32 encoding, size_t *consumed,
    const mobi_structured_toc_parser::ParseCallbacks &callbacks) {
  if (consumed)
    *consumed = 0;
  if (!entry || len == 0)
    return std::string();
  const size_t ident_len = (size_t)entry[0];
  if (1 + ident_len > len)
    return std::string();
  if (consumed)
    *consumed = 1 + ident_len;
  std::string raw_ident((const char *)entry + 1, ident_len);
  std::string out = callbacks.decode_bytes_to_utf8(raw_ident, encoding);
  return callbacks.normalize_title(out);
}

} // namespace

namespace mobi_structured_toc_parser {

bool ParseStructuredToc(const std::string &raw, const std::vector<u32> &offsets,
                        u32 ncx_index, u32 encoding,
                        const ParseCallbacks &callbacks,
                        std::vector<MobiStructuredTocEntry> *out,
                        IStatusReporter *reporter) {
  if (!out)
    return false;
  out->clear();
  if (!callbacks.decode_bytes_to_utf8 || !callbacks.normalize_title ||
      !callbacks.reject_title)
    return false;

  if (ncx_index == kMobiNullIndex || ncx_index + 1 >= offsets.size())
    return false;

  const u8 *main_rec = NULL;
  size_t main_len = 0;
  if (!GetMobiRecordSlice(raw, offsets, ncx_index, &main_rec, &main_len))
    return false;

  MobiIndxHeader main_hdr;
  if (!ParseMobiIndxHeader(main_rec, main_len, &main_hdr))
    return false;

  if (main_hdr.count == 0)
    return false;
  if ((size_t)ncx_index + (size_t)main_hdr.count + 1 >= offsets.size())
    return false;

  size_t tagx_start = (size_t)main_hdr.tagx;
  if (tagx_start + 4 > main_len ||
      memcmp(main_rec + tagx_start, "TAGX", 4) != 0) {
    size_t found =
        FindMobiSignature(main_rec, main_len, "TAGX", 4, main_hdr.header_end);
    if (found == SIZE_MAX)
      return false;
    tagx_start = found;
  }

  u32 control_bytes = 0;
  std::vector<MobiTagXRule> tag_rules;
  if (!ParseMobiTagxSection(main_rec, main_len, tagx_start, &control_bytes,
                            &tag_rules))
    return false;

  std::unordered_map<u32, std::string> cncx_text;
  if (main_hdr.ncncx > 0) {
    const u32 cncx_start = ncx_index + main_hdr.count + 1;
    ParseMobiCncxRecords(raw, offsets, cncx_start, main_hdr.ncncx, encoding,
                         &cncx_text, callbacks);
  }

  size_t with_pos = 0;
  for (u32 rec_idx = ncx_index + 1; rec_idx <= ncx_index + main_hdr.count;
       rec_idx++) {
    const u8 *rec = NULL;
    size_t rec_len = 0;
    if (!GetMobiRecordSlice(raw, offsets, rec_idx, &rec, &rec_len))
      continue;

    MobiIndxHeader hdr;
    if (!ParseMobiIndxHeader(rec, rec_len, &hdr))
      continue;
    if (hdr.start + 4 > rec_len || memcmp(rec + hdr.start, "IDXT", 4) != 0)
      continue;
    if (hdr.count == 0)
      continue;

    const size_t idxt_entries_end =
        (size_t)hdr.start + 4 + (size_t)hdr.count * 2;
    if (idxt_entries_end > rec_len)
      continue;

    std::vector<size_t> idx_positions;
    idx_positions.reserve((size_t)hdr.count + 1);
    for (u32 i = 0; i < hdr.count; i++) {
      const size_t pos =
          (size_t)mobi_record_scan::ReadBE16(rec + hdr.start + 4 + i * 2);
      if (pos >= rec_len || pos >= (size_t)hdr.start)
        continue;
      idx_positions.push_back(pos);
    }
    if (idx_positions.empty())
      continue;
    idx_positions.push_back((size_t)hdr.start);

    for (size_t i = 0; i + 1 < idx_positions.size(); i++) {
      const size_t start = idx_positions[i];
      const size_t end = idx_positions[i + 1];
      if (end <= start || end > rec_len)
        continue;

      size_t consumed = 0;
      std::string ident =
          DecodeMobiIndexIdent(rec + start, end - start, encoding, &consumed,
                               callbacks);
      if (consumed == 0 || start + consumed > end)
        continue;

      MobiTagMap tag_map;
      if (!ParseMobiTagMap(rec + start + consumed, end - (start + consumed),
                           control_bytes, tag_rules, &tag_map))
        continue;

      MobiStructuredTocEntry entry;
      entry.title = ident;
      entry.pos = kMobiNullIndex;
      entry.level = 0;

      std::unordered_map<u8, std::vector<u32>>::const_iterator it;
      it = tag_map.find(3); // Label offset in CNCX.
      if (it != tag_map.end() && !it->second.empty()) {
        std::unordered_map<u32, std::string>::const_iterator t =
            cncx_text.find(it->second[0]);
        if (t != cncx_text.end() && !t->second.empty())
          entry.title = t->second;
      }

      it = tag_map.find(1); // File position.
      if (it != tag_map.end() && !it->second.empty())
        entry.pos = it->second[0];
      if (entry.pos == kMobiNullIndex) {
        // Some generators store KF-style position tuples under tag 6.
        it = tag_map.find(6);
        if (it != tag_map.end() && it->second.size() >= 2)
          entry.pos = it->second[1];
      }

      it = tag_map.find(4); // Heading level.
      if (it != tag_map.end() && !it->second.empty()) {
        u32 lvl = it->second[0];
        entry.level = (lvl > 7) ? 7 : (u8)lvl;
      }

      entry.title = callbacks.normalize_title(entry.title);
      if (entry.title.empty() || callbacks.reject_title(entry.title))
        continue;

      if (!out->empty()) {
        const MobiStructuredTocEntry &last = out->back();
        if (last.pos == entry.pos && last.level == entry.level &&
            last.title == entry.title)
          continue;
      }

      if (entry.pos != kMobiNullIndex)
        with_pos++;
      out->push_back(entry);
    }
  }

  if (reporter && !out->empty()) {
    char msg[224];
    snprintf(msg, sizeof(msg),
             "MOBI: INDX structured entries=%u with_pos=%u cncx=%u",
             (unsigned)out->size(), (unsigned)with_pos,
             (unsigned)cncx_text.size());
    DBG_LOG(reporter, msg);
  }

  return !out->empty();
}

bool NcxLastRecord(const std::string &main_record, u32 ncx_index,
                   size_t record_count, u32 *last_record) {
  MobiIndxHeader hdr;
  if (!last_record || !ParseMobiIndxHeader((const u8 *)main_record.data(),
                                           main_record.size(), &hdr) ||
      hdr.count == 0 || (size_t)ncx_index + (size_t)hdr.count + 1 >= record_count)
    return false;
  u64 last = (u64)ncx_index + hdr.count;
  if (hdr.ncncx > 0)
    last += hdr.ncncx;
  *last_record = last >= record_count ? (u32)(record_count - 1) : (u32)last;
  return true;
}

} // namespace mobi_structured_toc_parser

