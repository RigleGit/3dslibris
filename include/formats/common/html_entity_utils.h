#pragma once

#include <cstdint>
#include <string>

namespace html_entity_utils {

struct ChunkedEntityNormalizerState {
  std::string carry;
};

bool DecodeHtmlEntityCodepoint(const std::string &entity, uint32_t *out);
bool DecodeHtmlEntityUtf8(const std::string &entity, std::string *out);
std::string NormalizeHtmlNamedEntitiesForXml(const std::string &in);
void NormalizeHtmlNamedEntitiesForXmlChunk(const char *chunk, size_t size, bool final,
                                           ChunkedEntityNormalizerState *state,
                                           std::string *out);

} // namespace html_entity_utils
