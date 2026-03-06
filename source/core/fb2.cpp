#include "fb2.h"

#include "stb_image.h"
#include <ctype.h>
#include <expat.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

static const size_t kFb2CoverBase64MaxChars = 3 * 1024 * 1024;
static const size_t kFb2CoverDecodedMaxBytes = 2 * 1024 * 1024;

static bool XmlNameEquals(const char *name, const char *needle) {
  if (!name || !needle)
    return false;
  if (!strcmp(name, needle))
    return true;
  const char *colon = strrchr(name, ':');
  return (colon && !strcmp(colon + 1, needle));
}

static std::string ExtractRefId(const char *href) {
  if (!href || !*href)
    return "";
  std::string s(href);
  size_t hash = s.find('#');
  if (hash == std::string::npos || hash + 1 >= s.size())
    return "";
  s = s.substr(hash + 1);
  while (!s.empty() && isspace((unsigned char)s.front()))
    s.erase(s.begin());
  while (!s.empty() && isspace((unsigned char)s.back()))
    s.pop_back();
  return s;
}

static const char *AttrValueByName(const char **attr, const char *name) {
  for (int i = 0; attr && attr[i]; i += 2) {
    if (XmlNameEquals(attr[i], name))
      return attr[i + 1];
  }
  return NULL;
}

struct CoverIdScanState {
  int coverpage_depth;
  std::string cover_id;
  std::string first_image_id;
};

static void cover_id_scan_start(void *userData, const char *el,
                                const char **attr) {
  CoverIdScanState *s = (CoverIdScanState *)userData;
  if (XmlNameEquals(el, "coverpage")) {
    s->coverpage_depth++;
    return;
  }
  if (!(XmlNameEquals(el, "image") || XmlNameEquals(el, "img")))
    return;

  const char *href = AttrValueByName(attr, "href");
  if (!href)
    href = AttrValueByName(attr, "src");
  std::string id = ExtractRefId(href);
  if (id.empty())
    return;

  if (s->first_image_id.empty())
    s->first_image_id = id;
  if (s->coverpage_depth > 0 && s->cover_id.empty())
    s->cover_id = id;
}

static void cover_id_scan_end(void *userData, const char *el) {
  CoverIdScanState *s = (CoverIdScanState *)userData;
  if (XmlNameEquals(el, "coverpage") && s->coverpage_depth > 0)
    s->coverpage_depth--;
}

struct BinaryScanState {
  std::string wanted_id;
  bool collecting;
  bool done;
  bool too_large;
  std::string base64;
};

static bool BinaryIdMatches(const char *raw, const std::string &wanted) {
  if (!raw || !*raw || wanted.empty())
    return false;
  if (!strcmp(raw, wanted.c_str()))
    return true;
  if (raw[0] == '#' && !strcmp(raw + 1, wanted.c_str()))
    return true;
  return false;
}

static void binary_scan_start(void *userData, const char *el, const char **attr) {
  BinaryScanState *s = (BinaryScanState *)userData;
  if (!XmlNameEquals(el, "binary"))
    return;

  const char *id = AttrValueByName(attr, "id");
  if (!BinaryIdMatches(id, s->wanted_id))
    return;
  s->collecting = true;
  s->base64.clear();
  s->done = false;
  s->too_large = false;
}

static void binary_scan_char(void *userData, const char *txt, int txtlen) {
  BinaryScanState *s = (BinaryScanState *)userData;
  if (!s->collecting || s->done || s->too_large || !txt || txtlen <= 0)
    return;

  for (int i = 0; i < txtlen; i++) {
    unsigned char c = (unsigned char)txt[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    s->base64.push_back((char)c);
  }
  if (s->base64.size() > kFb2CoverBase64MaxChars) {
    s->base64.clear();
    s->too_large = true;
    s->collecting = false;
  }
}

static void binary_scan_end(void *userData, const char *el) {
  BinaryScanState *s = (BinaryScanState *)userData;
  if (!XmlNameEquals(el, "binary"))
    return;
  if (s->collecting) {
    s->collecting = false;
    s->done = !s->base64.empty();
  }
}

static int Base64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+' || c == '-')
    return 62;
  if (c == '/' || c == '_')
    return 63;
  return -1;
}

static bool DecodeBase64Bytes(const std::string &in, std::vector<u8> *out,
                              size_t max_bytes) {
  if (!out)
    return false;
  out->clear();
  if (in.empty())
    return false;

  out->reserve((in.size() * 3) / 4);
  int accum = 0;
  int bits = -8;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '=')
      break;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    int v = Base64Value(c);
    if (v < 0)
      return false;
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 0) {
      out->push_back((u8)((accum >> bits) & 0xFF));
      bits -= 8;
      if (out->size() > max_bytes)
        return false;
    }
  }
  return !out->empty();
}

static bool ParsePass(const std::string &path, void *userData,
                      XML_StartElementHandler start,
                      XML_EndElementHandler end,
                      XML_CharacterDataHandler chardata,
                      bool (*done_check)(void *)) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
    return false;

  XML_Parser p = XML_ParserCreate(NULL);
  if (!p) {
    fclose(fp);
    return false;
  }
  XML_SetUserData(p, userData);
  XML_SetElementHandler(p, start, end);
  XML_SetCharacterDataHandler(p, chardata);

  char buf[8192];
  bool ok = true;
  while (true) {
    int n = (int)fread(buf, 1, sizeof(buf), fp);
    if (XML_Parse(p, buf, n, n == 0) == XML_STATUS_ERROR) {
      ok = false;
      break;
    }
    if (done_check && done_check(userData))
      break;
    if (n == 0)
      break;
  }

  XML_ParserFree(p);
  fclose(fp);
  return ok;
}

static bool binary_done_check(void *userData) {
  BinaryScanState *s = (BinaryScanState *)userData;
  return s->done || s->too_large;
}

static bool noop_done_check(void *) { return false; }

static int DecodeAndScaleToCover(Book *book, const u8 *data, int size) {
  if (!book || !data || size <= 0)
    return 10;

  int imgW = 0, imgH = 0, channels = 0;
  unsigned char *pixels = stbi_load_from_memory(data, size, &imgW, &imgH,
                                                 &channels, 3);
  if (!pixels)
    return 11;
  if (imgW <= 0 || imgH <= 0 || imgW > 2048 || imgH > 2048) {
    stbi_image_free(pixels);
    return 12;
  }

  const int thumbW = 85;
  const int thumbH = 115;
  float scaleX = (float)imgW / thumbW;
  float scaleY = (float)imgH / thumbH;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;
  if (scale < 1.0f)
    scale = 1.0f;
  int finalW = (int)(imgW / scale);
  int finalH = (int)(imgH / scale);
  if (finalW > thumbW)
    finalW = thumbW;
  if (finalH > thumbH)
    finalH = thumbH;
  if (finalW < 1)
    finalW = 1;
  if (finalH < 1)
    finalH = 1;

  if (book->coverPixels) {
    delete[] book->coverPixels;
    book->coverPixels = nullptr;
  }
  book->coverPixels = new u16[finalW * finalH];
  book->coverWidth = finalW;
  book->coverHeight = finalH;

  for (int y = 0; y < finalH; y++) {
    int srcY = (int)(y * scale);
    if (srcY >= imgH)
      srcY = imgH - 1;
    for (int x = 0; x < finalW; x++) {
      int srcX = (int)(x * scale);
      if (srcX >= imgW)
        srcX = imgW - 1;
      unsigned char *px = &pixels[(srcY * imgW + srcX) * 3];
      u16 r = (px[0] >> 3) & 0x1F;
      u16 g = (px[1] >> 2) & 0x3F;
      u16 b = (px[2] >> 3) & 0x1F;
      book->coverPixels[y * finalW + x] = (r << 11) | (g << 5) | b;
    }
  }

  stbi_image_free(pixels);
  return 0;
}

} // namespace

int fb2_extract_cover(Book *book, const std::string &fb2path) {
  if (!book || fb2path.empty())
    return 1;

  CoverIdScanState ids;
  ids.coverpage_depth = 0;
  if (!ParsePass(fb2path, &ids, cover_id_scan_start, cover_id_scan_end, NULL,
                 noop_done_check)) {
    return 2;
  }

  std::string wanted_id = ids.cover_id.empty() ? ids.first_image_id : ids.cover_id;
  if (wanted_id.empty())
    return 3;

  BinaryScanState bin;
  bin.wanted_id = wanted_id;
  bin.collecting = false;
  bin.done = false;
  bin.too_large = false;
  if (!ParsePass(fb2path, &bin, binary_scan_start, binary_scan_end,
                 binary_scan_char, binary_done_check)) {
    return 4;
  }
  if (!bin.done || bin.base64.empty())
    return 5;

  std::vector<u8> decoded;
  if (!DecodeBase64Bytes(bin.base64, &decoded, kFb2CoverDecodedMaxBytes))
    return 6;

  return DecodeAndScaleToCover(book, decoded.data(), (int)decoded.size());
}
