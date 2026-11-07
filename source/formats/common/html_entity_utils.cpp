#include "formats/common/html_entity_utils.h"

#include "formats/common/text_helpers.h"

#include <cstdio>
#include <cstring>

namespace html_entity_utils {
namespace {

struct NamedEntity {
  const char *name;
  uint32_t codepoint;
};

const NamedEntity kNamedEntities[] = {
    {"quot", 0x0022},  {"amp", 0x0026},    {"apos", 0x0027},
    {"lt", 0x003C},    {"gt", 0x003E},     {"nbsp", 0x00A0},
    {"iexcl", 0x00A1}, {"cent", 0x00A2},   {"pound", 0x00A3},
    {"curren", 0x00A4},{"yen", 0x00A5},    {"brvbar", 0x00A6},
    {"sect", 0x00A7},  {"uml", 0x00A8},    {"copy", 0x00A9},
    {"ordf", 0x00AA},  {"laquo", 0x00AB},  {"not", 0x00AC},
    {"shy", 0x00AD},   {"reg", 0x00AE},    {"macr", 0x00AF},
    {"deg", 0x00B0},   {"plusmn", 0x00B1}, {"sup2", 0x00B2},
    {"sup3", 0x00B3},  {"acute", 0x00B4},  {"micro", 0x00B5},
    {"para", 0x00B6},  {"middot", 0x00B7}, {"cedil", 0x00B8},
    {"sup1", 0x00B9},  {"ordm", 0x00BA},   {"raquo", 0x00BB},
    {"frac14", 0x00BC},{"frac12", 0x00BD}, {"frac34", 0x00BE},
    {"iquest", 0x00BF},{"Agrave", 0x00C0}, {"Aacute", 0x00C1},
    {"Acirc", 0x00C2}, {"Atilde", 0x00C3}, {"Auml", 0x00C4},
    {"Aring", 0x00C5}, {"AElig", 0x00C6},  {"Ccedil", 0x00C7},
    {"Egrave", 0x00C8},{"Eacute", 0x00C9}, {"Ecirc", 0x00CA},
    {"Euml", 0x00CB},  {"Igrave", 0x00CC}, {"Iacute", 0x00CD},
    {"Icirc", 0x00CE}, {"Iuml", 0x00CF},   {"ETH", 0x00D0},
    {"Ntilde", 0x00D1},{"Ograve", 0x00D2}, {"Oacute", 0x00D3},
    {"Ocirc", 0x00D4}, {"Otilde", 0x00D5}, {"Ouml", 0x00D6},
    {"times", 0x00D7}, {"Oslash", 0x00D8}, {"Ugrave", 0x00D9},
    {"Uacute", 0x00DA},{"Ucirc", 0x00DB},  {"Uuml", 0x00DC},
    {"Yacute", 0x00DD},{"THORN", 0x00DE},  {"szlig", 0x00DF},
    {"agrave", 0x00E0},{"aacute", 0x00E1}, {"acirc", 0x00E2},
    {"atilde", 0x00E3},{"auml", 0x00E4},   {"aring", 0x00E5},
    {"aelig", 0x00E6}, {"ccedil", 0x00E7}, {"egrave", 0x00E8},
    {"eacute", 0x00E9},{"ecirc", 0x00EA},  {"euml", 0x00EB},
    {"igrave", 0x00EC},{"iacute", 0x00ED}, {"icirc", 0x00EE},
    {"iuml", 0x00EF},  {"eth", 0x00F0},    {"ntilde", 0x00F1},
    {"ograve", 0x00F2},{"oacute", 0x00F3}, {"ocirc", 0x00F4},
    {"otilde", 0x00F5},{"ouml", 0x00F6},   {"divide", 0x00F7},
    {"oslash", 0x00F8},{"ugrave", 0x00F9}, {"uacute", 0x00FA},
    {"ucirc", 0x00FB}, {"uuml", 0x00FC},   {"yacute", 0x00FD},
    {"thorn", 0x00FE}, {"yuml", 0x00FF},   {"ndash", 0x2013},
    {"mdash", 0x2014}, {"lsquo", 0x2018},  {"rsquo", 0x2019},
    {"sbquo", 0x201A}, {"ldquo", 0x201C},  {"rdquo", 0x201D},
    {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021},
    {"bull", 0x2022},  {"hellip", 0x2026}, {"permil", 0x2030},
    {"prime", 0x2032}, {"Prime", 0x2033},  {"lsaquo", 0x2039},
    {"rsaquo", 0x203A},{"oline", 0x203E},  {"frasl", 0x2044},
    {"euro", 0x20AC},  {"trade", 0x2122},  {"minus", 0x2212},
};

static bool IsValidUnicodeScalar(uint32_t cp) {
  return cp > 0 && cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
}

static bool IsPredefinedXmlEntity(const std::string &token) {
  return token == "amp" || token == "lt" || token == "gt" ||
         token == "quot" || token == "apos";
}

static std::string StripEntityDelimiters(const std::string &entity) {
  size_t start = 0;
  size_t end = entity.size();
  if (start < end && entity[start] == '&')
    start++;
  if (start < end && entity[end - 1] == ';')
    end--;
  return entity.substr(start, end - start);
}

static void AppendNormalizedEntityToken(const std::string &full,
                                        std::string *out) {
  if (!out) {
    return;
  }
  const std::string token = StripEntityDelimiters(full);
  if (token.empty()) {
    *out += full;
    return;
  }
  if (token[0] == '#' || IsPredefinedXmlEntity(token)) {
    *out += full;
    return;
  }

  uint32_t cp = 0;
  if (!DecodeHtmlEntityCodepoint(token, &cp)) {
    *out += full;
    return;
  }

  char numeric[24];
  std::snprintf(numeric, sizeof(numeric), "&#%u;", (unsigned)cp);
  *out += numeric;
}

} // namespace

bool DecodeHtmlEntityCodepoint(const std::string &entity, uint32_t *out) {
  if (!out)
    return false;
  *out = 0;

  const std::string token = StripEntityDelimiters(entity);
  if (token.empty())
    return false;

  if (token[0] == '#') {
    unsigned long parsed = 0;
    if (token.size() >= 2 && (token[1] == 'x' || token[1] == 'X')) {
      if (std::sscanf(token.c_str() + 2, "%lx", &parsed) != 1)
        return false;
    } else {
      if (std::sscanf(token.c_str() + 1, "%lu", &parsed) != 1)
        return false;
    }
    if (!IsValidUnicodeScalar((uint32_t)parsed))
      return false;
    *out = (uint32_t)parsed;
    return true;
  }

  for (size_t i = 0; i < sizeof(kNamedEntities) / sizeof(kNamedEntities[0]);
       ++i) {
    if (!std::strcmp(token.c_str(), kNamedEntities[i].name)) {
      *out = kNamedEntities[i].codepoint;
      return true;
    }
  }

  return false;
}

bool DecodeHtmlEntityUtf8(const std::string &entity, std::string *out) {
  if (!out)
    return false;

  uint32_t cp = 0;
  if (!DecodeHtmlEntityCodepoint(entity, &cp))
    return false;

  AppendUtf8Codepoint(out, cp);
  return true;
}

std::string NormalizeHtmlNamedEntitiesForXml(const std::string &in) {
  std::string out;
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out.push_back(in[i]);
      continue;
    }

    size_t semi = in.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 24) {
      out.push_back(in[i]);
      continue;
    }

    const std::string full = in.substr(i, semi - i + 1);
    const std::string token = StripEntityDelimiters(full);
    if (token.empty()) {
      out.push_back(in[i]);
      continue;
    }

    if (token[0] == '#' || IsPredefinedXmlEntity(token)) {
      out += full;
      i = semi;
      continue;
    }

    uint32_t cp = 0;
    if (!DecodeHtmlEntityCodepoint(token, &cp)) {
      out += full;
      i = semi;
      continue;
    }

    char numeric[24];
    std::snprintf(numeric, sizeof(numeric), "&#%u;", (unsigned)cp);
    out += numeric;
    i = semi;
  }

  return out;
}

void NormalizeHtmlNamedEntitiesForXmlChunk(const char *chunk, size_t size, bool final,
                                           ChunkedEntityNormalizerState *state,
                                           std::string *out) {
  if (!state || !out)
    return;

  std::string joined;
  if (!state->carry.empty()) {
    joined.swap(state->carry);
    joined.append(chunk, size);
    chunk = joined.data();
    size = joined.size();
  }
  size_t i = 0;
  while (i < size) {
    const char *amp = static_cast<const char *>(memchr(chunk + i, '&', size - i));
    const size_t next = amp ? (size_t)(amp - chunk) : size;
    out->append(chunk + i, next - i);
    if (!amp)
      return;
    i = next;
    const char *semi = static_cast<const char *>(memchr(amp + 1, ';', size - i - 1));
    if (!semi) {
      if (final)
        out->append(amp, size - i);
      else
        state->carry.assign(amp, size - i);
      return;
    }
    if (semi - amp > 24) {
      out->push_back('&');
      ++i;
      continue;
    }
    AppendNormalizedEntityToken(std::string(amp, (size_t)(semi - amp + 1)), out);
    i = (size_t)(semi - chunk) + 1;
  }
}

} // namespace html_entity_utils
