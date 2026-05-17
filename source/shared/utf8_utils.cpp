#include "shared/utf8_utils.h"

#include <cstring>
#include <vector>

namespace utf8_utils {

void AppendUtf8Codepoint(std::string *out, uint32_t cp) {
  if (!out)
    return;
  if (cp <= 0x7F) {
    out->push_back((char)cp);
  } else if (cp <= 0x7FF) {
    out->push_back((char)(0xC0 | (cp >> 6)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

namespace {

void AppendCp1252Byte(std::string *out, unsigned char b) {
  static const uint16_t kCp1252Map[32] = {
      0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
      0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
  };

  if (!out)
    return;
  if (b < 0x80) {
    out->push_back((char)b);
    return;
  }
  if (b <= 0x9F) {
    uint16_t mapped = kCp1252Map[b - 0x80];
    if (mapped != 0x0000)
      AppendUtf8Codepoint(out, mapped);
    else
      out->push_back('?');
    return;
  }
  AppendUtf8Codepoint(out, b);
}

} // namespace

bool IsValidUtf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else
      return false;

    if (i + need >= s.size())
      return false;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)s[i + j];
      if ((cc & 0xC0) != 0x80)
        return false;
    }
    i += need + 1;
  }
  return true;
}

bool IsValidUtf8(const char *s) {
  if (!s)
    return false;
  return IsValidUtf8(std::string(s));
}

std::string DecodeCp1252ToUtf8(const std::string &in) {
  std::string out;
  out.reserve(in.size() * 2);
  for (size_t i = 0; i < in.size(); i++)
    AppendCp1252Byte(&out, (unsigned char)in[i]);
  return out;
}

bool TryRepairMojibakeUtf8(const std::string &in, std::string *out) {
  if (!out)
    return false;

  bool has_markers = false;
  for (size_t i = 0; i + 1 < in.size(); i++) {
    unsigned char c0 = (unsigned char)in[i];
    unsigned char c1 = (unsigned char)in[i + 1];
    if (c0 == 0xC3 && (c1 == 0x82 || c1 == 0x83)) {
      has_markers = true;
      break;
    }
  }
  if (!has_markers)
    return false;

  std::string collapsed;
  collapsed.reserve(in.size());
  bool changed = false;
  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x80) {
      collapsed.push_back((char)c);
      i++;
      continue;
    }
    if (i + 1 >= in.size())
      return false;

    unsigned char c2 = (unsigned char)in[i + 1];
    if (c == 0xC2 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)c2);
      changed = true;
      i += 2;
      continue;
    }
    if (c == 0xC3 && c2 >= 0x80 && c2 <= 0xBF) {
      collapsed.push_back((char)(c2 + 0x40));
      changed = true;
      i += 2;
      continue;
    }

    size_t step = 0;
    if ((c & 0xE0) == 0xC0)
      step = 2;
    else if ((c & 0xF0) == 0xE0)
      step = 3;
    else if ((c & 0xF8) == 0xF0)
      step = 4;

    if (step > 1 && i + step <= in.size()) {
      bool ok = true;
      for (size_t j = 1; j < step; j++) {
        unsigned char cc = (unsigned char)in[i + j];
        if ((cc & 0xC0) != 0x80) {
          ok = false;
          break;
        }
      }
      if (ok) {
        collapsed.append(in, i, step);
        i += step;
        continue;
      }
    }

    collapsed.push_back((char)c);
    i++;
  }

  if (!changed)
    return false;
  if (!IsValidUtf8(collapsed))
    collapsed = DecodeCp1252ToUtf8(collapsed);
  if (!IsValidUtf8(collapsed))
    return false;
  *out = collapsed;
  return true;
}

bool TryRepairFullwidthByteMojibake(const std::string &in, std::string *out) {
  if (!out || in.empty())
    return false;

  std::string collapsed;
  collapsed.reserve(in.size());
  std::vector<unsigned char> mapped;
  mapped.reserve(in.size() / 3);
  bool changed = false;

  for (size_t i = 0; i < in.size();) {
    unsigned char b0 = (unsigned char)in[i];
    if (i + 2 < in.size() && b0 == 0xEF) {
      unsigned char b1 = (unsigned char)in[i + 1];
      unsigned char b2 = (unsigned char)in[i + 2];
      if (b1 >= 0xBC && b1 <= 0xBF && b2 >= 0x80 && b2 <= 0xBF) {
        unsigned char recovered =
            (unsigned char)(((b1 - 0xBC) << 6) | (b2 - 0x80));
        collapsed.push_back((char)recovered);
        mapped.push_back(recovered);
        i += 3;
        changed = true;
        continue;
      }
    }
    collapsed.push_back((char)b0);
    i++;
  }

  if (!changed || mapped.size() < 2)
    return false;

  bool has_utf8_pair = false;
  for (size_t i = 0; i + 1 < mapped.size(); i++) {
    unsigned char lead = mapped[i];
    unsigned char cont = mapped[i + 1];
    if (lead >= 0xC2 && lead <= 0xF4 && cont >= 0x80 && cont <= 0xBF) {
      has_utf8_pair = true;
      break;
    }
  }
  if (!has_utf8_pair || !IsValidUtf8(collapsed))
    return false;

  *out = collapsed;
  return true;
}

std::string ComposeLatinCombiningMarks(const std::string &in) {
  if (in.empty())
    return in;

  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (i + 2 < in.size() &&
        (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'A' ||
         c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'n' ||
         c == 'N') &&
        (unsigned char)in[i + 1] == 0xCC) {
      unsigned char mark = (unsigned char)in[i + 2];
      const char *rep = NULL;
      if (mark == 0x81) {
        switch (c) {
        case 'a': rep = "\xC3\xA1"; break;
        case 'e': rep = "\xC3\xA9"; break;
        case 'i': rep = "\xC3\xAD"; break;
        case 'o': rep = "\xC3\xB3"; break;
        case 'u': rep = "\xC3\xBA"; break;
        case 'A': rep = "\xC3\x81"; break;
        case 'E': rep = "\xC3\x89"; break;
        case 'I': rep = "\xC3\x8D"; break;
        case 'O': rep = "\xC3\x93"; break;
        case 'U': rep = "\xC3\x9A"; break;
        }
      } else if (mark == 0x83) {
        switch (c) {
        case 'n': rep = "\xC3\xB1"; break;
        case 'N': rep = "\xC3\x91"; break;
        }
      } else if (mark == 0x88) {
        switch (c) {
        case 'u': rep = "\xC3\xBC"; break;
        case 'U': rep = "\xC3\x9C"; break;
        }
      }
      if (rep) {
        out.append(rep);
        i += 2;
        continue;
      }
    }
    out.push_back((char)c);
  }
  return out;
}

bool Utf16NameToUtf8(const uint16_t *name, std::string *out, size_t max_units) {
  if (!name || !out)
    return false;

  out->clear();
  size_t i = 0;
  while (i < max_units && name[i] != 0) {
    uint32_t cp = name[i++];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i >= max_units || name[i] == 0)
        return false;
      uint32_t low = name[i];
      if (low < 0xDC00 || low > 0xDFFF)
        return false;
      i++;
      cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      return false;
    }
    AppendUtf8Codepoint(out, cp);
  }
  return true;
}

std::string NormalizeFsFilenameForIo(const std::string &raw_name) {
  std::string repaired;
  if (TryRepairFullwidthByteMojibake(raw_name, &repaired))
    return ComposeLatinCombiningMarks(repaired);
  return ComposeLatinCombiningMarks(raw_name);
}

size_t CountUtf8InvalidLeadBytes(const std::string &bytes) {
  size_t invalid = 0;
  size_t i = 0;
  while (i < bytes.size()) {
    unsigned char c = (unsigned char)bytes[i];
    if ((c & 0x80) == 0x00) {
      i++;
      continue;
    }
    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;
    else {
      invalid++;
      i++;
      continue;
    }
    if (i + need >= bytes.size()) {
      invalid++;
      i++;
      continue;
    }
    bool ok = true;
    for (size_t j = 1; j <= need; j++) {
      unsigned char cc = (unsigned char)bytes[i + j];
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      invalid++;
      i++;
      continue;
    }
    i += need + 1;
  }
  return invalid;
}

std::string DecodeMostlyUtf8WithCp1252Fallback(const std::string &in,
                                               size_t *invalid_out) {
  if (invalid_out)
    *invalid_out = 0;
  std::string out;
  out.reserve(in.size() * 2);

  size_t i = 0;
  while (i < in.size()) {
    unsigned char c = (unsigned char)in[i];
    if ((c & 0x80) == 0x00) {
      out.push_back((char)c);
      i++;
      continue;
    }

    size_t need = 0;
    if ((c & 0xE0) == 0xC0)
      need = 1;
    else if ((c & 0xF0) == 0xE0)
      need = 2;
    else if ((c & 0xF8) == 0xF0)
      need = 3;

    bool valid = (need > 0 && i + need < in.size());
    if (valid) {
      for (size_t j = 1; j <= need; j++) {
        unsigned char cc = (unsigned char)in[i + j];
        if ((cc & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
    }

    if (valid) {
      out.append(in, i, need + 1);
      i += need + 1;
      continue;
    }

    AppendCp1252Byte(&out, c);
    if (invalid_out)
      (*invalid_out)++;
    i++;
  }

  return out;
}

} // namespace utf8_utils
