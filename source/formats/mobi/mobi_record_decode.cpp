#include "formats/mobi/mobi_record_decode.h"

#include <algorithm>
#include <string.h>

namespace mobi_record_decode {
namespace {

struct HuffTable1Entry {
  bool found;
  uint8_t code_length;
  uint32_t value;
};

struct HuffTable2Entry {
  uint32_t first;
  uint32_t value;
};

struct HuffDictionaryEntry {
  std::string value;
  bool decompressed;
};

struct HuffCdicDecoder {
  std::vector<HuffTable1Entry> table1;
  std::vector<HuffTable2Entry> table2;
  std::vector<HuffDictionaryEntry> dictionary;
  bool ready;

  HuffCdicDecoder() : table1(256), table2(33), ready(false) {}
};

static uint16_t ReadBE16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t ReadBE32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool GetRecordSlice(const std::string &raw,
                           const std::vector<uint32_t> &offsets,
                           uint32_t record_index, const uint8_t **data,
                           size_t *len) {
  if (!data || !len || record_index + 1 >= offsets.size())
    return false;
  uint32_t start = offsets[(size_t)record_index];
  uint32_t end = offsets[(size_t)record_index + 1];
  if (end <= start || end > raw.size())
    return false;
  *data = (const uint8_t *)raw.data() + start;
  *len = (size_t)(end - start);
  return true;
}

static bool DecompressPalmDocRecord(const uint8_t *src, size_t src_len,
                                    std::string *out) {
  if (!src || !out)
    return false;
  out->clear();
  out->reserve(src_len * 2);

  size_t i = 0;
  while (i < src_len) {
    uint8_t c = src[i++];
    if (c == 0 || (c >= 0x09 && c <= 0x7F)) {
      out->push_back((char)c);
      continue;
    }
    if (c >= 0x01 && c <= 0x08) {
      size_t n = (size_t)c;
      if (i + n > src_len)
        n = src_len - i;
      out->append((const char *)(src + i), n);
      i += n;
      continue;
    }
    if (c >= 0xC0) {
      out->push_back(' ');
      out->push_back((char)(c ^ 0x80));
      continue;
    }
    if (i >= src_len)
      break;

    uint8_t c2 = src[i++];
    uint16_t pair = (uint16_t)(((uint16_t)c << 8) | (uint16_t)c2);
    int distance = (pair >> 3) & 0x07FF;
    int length = (pair & 0x0007) + 3;
    if (distance <= 0 || (size_t)distance > out->size())
      continue;
    for (int j = 0; j < length; j++) {
      size_t idx = out->size() - (size_t)distance;
      out->push_back((*out)[idx]);
    }
  }
  return true;
}

static uint32_t Read32Bits(const uint8_t *data, size_t len, size_t from_bit) {
  size_t start_byte = from_bit >> 3;
  size_t end_bit = from_bit + 32;
  size_t end_byte = end_bit >> 3;
  uint64_t bits = 0;
  for (size_t i = start_byte; i <= end_byte; i++) {
    bits <<= 8;
    if (i < len)
      bits |= (uint64_t)data[i];
  }
  size_t shift = 8 - (end_bit & 7);
  return (uint32_t)((bits >> shift) & 0xFFFFFFFFu);
}

static bool DecodeHuffBits(HuffCdicDecoder *decoder, const uint8_t *src,
                           size_t src_len, std::string *out);

static bool ExpandDictionaryEntry(HuffCdicDecoder *decoder, size_t index,
                                  std::string *out) {
  if (!decoder || !out || index >= decoder->dictionary.size())
    return false;
  HuffDictionaryEntry &entry = decoder->dictionary[index];
  if (entry.decompressed) {
    *out = entry.value;
    return true;
  }
  std::string expanded;
  if (!DecodeHuffBits(decoder, (const uint8_t *)entry.value.data(),
                      entry.value.size(), &expanded))
    return false;
  entry.value.swap(expanded);
  entry.decompressed = true;
  *out = entry.value;
  return true;
}

static bool DecodeHuffBits(HuffCdicDecoder *decoder, const uint8_t *src,
                           size_t src_len, std::string *out) {
  if (!decoder || !decoder->ready || !src || !out)
    return false;

  out->clear();
  size_t bit_length = src_len * 8;
  for (size_t bit = 0; bit < bit_length;) {
    uint32_t bits = Read32Bits(src, src_len, bit);
    const HuffTable1Entry &quick = decoder->table1[bits >> 24];
    uint8_t code_length = quick.code_length;
    uint32_t value = quick.value;
    if (!quick.found) {
      while (code_length < decoder->table2.size() &&
             (bits >> (32 - code_length)) < decoder->table2[code_length].first)
        code_length++;
      if (code_length == 0 || code_length >= decoder->table2.size())
        return false;
      value = decoder->table2[code_length].value;
    }

    bit += code_length;
    if (bit > bit_length)
      break;

    uint32_t prefix = bits >> (32 - code_length);
    if (value < prefix)
      return false;
    uint32_t code = value - prefix;
    std::string decoded;
    if (!ExpandDictionaryEntry(decoder, code, &decoded))
      return false;
    *out += decoded;
  }
  return true;
}

static bool InitHuffCdicDecoder(const std::string &raw,
                                const std::vector<uint32_t> &offsets,
                                const MobiRecord0Header &header,
                                HuffCdicDecoder *out) {
  if (!out || header.huffcdic_record_index == 0 ||
      header.huffcdic_record_index == kNullIndex ||
      header.num_huffcdic_records < 2)
    return false;

  const uint8_t *huff = NULL;
  size_t huff_len = 0;
  if (!GetRecordSlice(raw, offsets, header.huffcdic_record_index, &huff,
                      &huff_len) ||
      huff_len < 16 || memcmp(huff, "HUFF", 4) != 0)
    return false;

  uint32_t offset1 = ReadBE32(huff + 8);
  uint32_t offset2 = ReadBE32(huff + 12);
  if (offset1 + 256 * 4 > huff_len || offset2 + 32 * 8 > huff_len)
    return false;

  out->table1.assign(256, HuffTable1Entry());
  for (size_t i = 0; i < 256; i++) {
    uint32_t x = ReadBE32(huff + offset1 + i * 4);
    out->table1[i].found = (x & 0x80u) != 0;
    out->table1[i].code_length = (uint8_t)(x & 0x1Fu);
    out->table1[i].value = x >> 8;
  }

  out->table2.assign(33, HuffTable2Entry());
  for (size_t i = 1; i <= 32; i++) {
    out->table2[i].first = ReadBE32(huff + offset2 + (i - 1) * 8);
    out->table2[i].value = ReadBE32(huff + offset2 + (i - 1) * 8 + 4);
  }

  out->dictionary.clear();
  for (uint32_t i = 1; i < header.num_huffcdic_records; i++) {
    const uint8_t *cdic = NULL;
    size_t cdic_len = 0;
    if (!GetRecordSlice(raw, offsets, header.huffcdic_record_index + i, &cdic,
                        &cdic_len) ||
        cdic_len < 16 || memcmp(cdic, "CDIC", 4) != 0)
      return false;

    uint32_t table_len = ReadBE32(cdic + 4);
    uint32_t num_entries = ReadBE32(cdic + 8);
    uint32_t code_length = ReadBE32(cdic + 12);
    if (table_len > cdic_len || code_length >= 31)
      return false;

    const uint8_t *buffer = cdic + table_len;
    size_t buffer_len = cdic_len - table_len;
    uint32_t remaining = 0;
    if (num_entries > out->dictionary.size())
      remaining = num_entries - (uint32_t)out->dictionary.size();
    uint32_t n = std::min<uint32_t>(1u << code_length, remaining);
    for (uint32_t entry = 0; entry < n; entry++) {
      size_t index_off = (size_t)entry * 2;
      if (index_off + 2 > buffer_len)
        return false;
      uint16_t entry_off = ReadBE16(buffer + index_off);
      if ((size_t)entry_off + 2 > buffer_len)
        return false;
      uint16_t x = ReadBE16(buffer + entry_off);
      size_t length = (size_t)(x & 0x7FFFu);
      bool decompressed = (x & 0x8000u) != 0;
      if ((size_t)entry_off + 2 + length > buffer_len)
        return false;
      out->dictionary.push_back(
          {std::string((const char *)buffer + entry_off + 2, length),
           decompressed});
    }
  }

  out->ready = !out->dictionary.empty();
  return out->ready;
}

static bool DecodeTextRecord(const uint8_t *src, size_t src_len,
                             const MobiRecord0Header &header,
                             HuffCdicDecoder *huff, std::string *out) {
  if (!src || !out)
    return false;
  std::string trimmed =
      RemoveTrailingEntries(src, src_len, header.trailing_flags);
  if (header.compression == 1) {
    *out = trimmed;
    return true;
  }
  if (header.compression == 2) {
    return DecompressPalmDocRecord((const uint8_t *)trimmed.data(), trimmed.size(),
                                   out);
  }
  if (header.compression == 17480)
    return DecodeHuffBits(huff, (const uint8_t *)trimmed.data(), trimmed.size(),
                          out);
  return false;
}

} // namespace

MobiRecord0Header::MobiRecord0Header()
    : compression(0), text_len(0), text_rec_count(0), encoding(1252),
      resource_start(0), title_offset(0), title_length(0),
      huffcdic_record_index(0), num_huffcdic_records(0), trailing_flags(0),
      indx_index(kNullIndex) {}

bool ParseRecord0Header(const uint8_t *rec0, size_t rec0_len,
                        MobiRecord0Header *out) {
  if (!rec0 || !out || rec0_len < 16)
    return false;

  *out = MobiRecord0Header();
  out->compression = ReadBE16(rec0 + 0);
  out->text_len = ReadBE32(rec0 + 4);
  out->text_rec_count = ReadBE16(rec0 + 8);

  if (rec0_len < 20 || memcmp(rec0 + 16, "MOBI", 4) != 0)
    return true;

  if (rec0_len >= 32)
    out->encoding = ReadBE32(rec0 + 28);
  if (rec0_len >= 92) {
    out->title_offset = ReadBE32(rec0 + 84);
    out->title_length = ReadBE32(rec0 + 88);
  }
  if (rec0_len >= 112)
    out->resource_start = ReadBE32(rec0 + 108);
  if (rec0_len >= 120) {
    out->huffcdic_record_index = ReadBE32(rec0 + 112);
    out->num_huffcdic_records = ReadBE32(rec0 + 116);
  }
  if (rec0_len >= 244)
    out->trailing_flags = ReadBE32(rec0 + 240);
  if (rec0_len >= 248)
    out->indx_index = ReadBE32(rec0 + 244);
  return true;
}

size_t CountBitsSet(uint32_t x) {
  size_t count = 0;
  while (x > 0) {
    if ((x & 1u) != 0)
      count++;
    x >>= 1;
  }
  return count;
}

uint32_t GetVarLenFromEnd(const uint8_t *data, size_t len) {
  if (!data || len == 0)
    return 0;
  uint32_t value = 0;
  size_t start = len > 4 ? len - 4 : 0;
  for (size_t i = start; i < len; i++) {
    uint8_t byte = data[i];
    if ((byte & 0x80u) != 0)
      value = 0;
    value = (value << 7) | (uint32_t)(byte & 0x7Fu);
  }
  return value;
}

std::string RemoveTrailingEntries(const uint8_t *data, size_t len,
                                  uint32_t trailing_flags) {
  if (!data || len == 0)
    return std::string();
  size_t keep = len;
  const bool multibyte = (trailing_flags & 1u) != 0;
  size_t trailing_entries = CountBitsSet(trailing_flags >> 1);
  for (size_t i = 0; i < trailing_entries && keep > 0; i++) {
    uint32_t trim = GetVarLenFromEnd(data, keep);
    if (trim == 0 || trim > keep)
      break;
    keep -= (size_t)trim;
  }
  if (multibyte && keep > 0) {
    size_t trim = (size_t)((data[keep - 1] & 0x03u) + 1u);
    if (trim <= keep)
      keep -= trim;
    else
      keep = 0;
  }
  return std::string((const char *)data, keep);
}

bool BuildMergedText(const std::string &raw, const std::vector<uint32_t> &offsets,
                     const MobiRecord0Header &header, std::string *merged) {
  if (!merged)
    return false;
  merged->clear();
  merged->reserve(header.text_len > 0 ? (size_t)header.text_len : 1024 * 1024);

  HuffCdicDecoder huff;
  HuffCdicDecoder *decoder = NULL;
  if (header.compression == 17480) {
    if (!InitHuffCdicDecoder(raw, offsets, header, &huff))
      return false;
    decoder = &huff;
  }

  const uint8_t *data = (const uint8_t *)raw.data();
  for (uint32_t rec = 1; rec <= header.text_rec_count; rec++) {
    if (rec + 1 >= offsets.size())
      return false;
    uint32_t start = offsets[(size_t)rec];
    uint32_t end = offsets[(size_t)rec + 1];
    if (end <= start || end > raw.size())
      continue;
    std::string text;
    if (!DecodeTextRecord(data + start, (size_t)(end - start), header, decoder,
                          &text))
      return false;
    *merged += text;
  }

  if (header.text_len > 0 && merged->size() > header.text_len)
    merged->resize((size_t)header.text_len);
  return true;
}

} // namespace mobi_record_decode
