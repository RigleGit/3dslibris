#include "formats/common/xml_parse_utils.h"

#include <algorithm>
#include <stdio.h>
#include <vector>

namespace xml_parse_utils {
namespace {

static XmlParseResult BuildParseError(XML_Parser parser, size_t bytes_consumed) {
  XmlParseResult result;
  result.ok = false;
  result.error_code = parser ? XML_GetErrorCode(parser) : XML_ERROR_NO_MEMORY;
  result.error_line = parser ? (int)XML_GetCurrentLineNumber(parser) : 0;
  result.error_column = parser ? (int)XML_GetCurrentColumnNumber(parser) : 0;
  result.bytes_consumed = bytes_consumed;
  return result;
}

static XmlParseResult BuildCustomError(XML_Parser parser, enum XML_Error code,
                                       size_t bytes_consumed) {
  XmlParseResult result;
  result.ok = false;
  result.error_code = code;
  result.error_line = parser ? (int)XML_GetCurrentLineNumber(parser) : 0;
  result.error_column = parser ? (int)XML_GetCurrentColumnNumber(parser) : 0;
  result.bytes_consumed = bytes_consumed;
  return result;
}

static XML_Parser CreateParser(const XmlParserOptions &options) {
  XML_Parser parser = XML_ParserCreate(NULL);
  if (!parser)
    return NULL;

  XML_SetUserData(parser, options.user_data);
  XML_SetElementHandler(parser, options.start_element, options.end_element);
  XML_SetCharacterDataHandler(parser, options.character_data);
  if (options.default_handler)
    XML_SetDefaultHandler(parser, options.default_handler);
  if (options.processing_instruction) {
    XML_SetProcessingInstructionHandler(parser,
                                        options.processing_instruction);
  }
  if (options.unknown_encoding) {
    XML_SetUnknownEncodingHandler(parser, options.unknown_encoding,
                                  options.unknown_encoding_data);
  }
  return parser;
}

static bool ShouldAbortParse(const XmlParserOptions &options) {
  return options.abort_parse &&
         options.abort_parse(options.abort_user_data ? options.abort_user_data
                                                    : options.user_data);
}

} // namespace

XmlParseResult ParseXmlFileStream(FILE *fp, const XmlParserOptions &options,
                                  size_t chunk_size,
                                  XmlShouldStopFn should_stop) {
  if (!fp || chunk_size == 0) {
    XmlParseResult result;
    result.ok = false;
    result.error_code = XML_ERROR_INVALID_ARGUMENT;
    return result;
  }

  XML_Parser parser = CreateParser(options);
  if (!parser)
    return BuildParseError(NULL, 0);

  XmlParseResult result;
  while (true) {
    if (ShouldAbortParse(options)) {
      result = BuildCustomError(parser, XML_ERROR_ABORTED,
                                result.bytes_consumed);
      break;
    }
    void *buffer = XML_GetBuffer(parser, (int)chunk_size);
    if (!buffer) {
      result = BuildParseError(parser, result.bytes_consumed);
      break;
    }

    size_t bytes_read = fread(buffer, 1, chunk_size, fp);
    if (bytes_read == 0 && ferror(fp)) {
      result =
          BuildCustomError(parser, XML_ERROR_EXTERNAL_ENTITY_HANDLING,
                           result.bytes_consumed);
      break;
    }
    enum XML_Status status =
        XML_ParseBuffer(parser, (int)bytes_read, bytes_read == 0);
    result.bytes_consumed += bytes_read;
    if (status == XML_STATUS_ERROR) {
      result = BuildParseError(parser, result.bytes_consumed);
      break;
    }
    if (should_stop && should_stop(options.user_data))
      break;
    if (ShouldAbortParse(options)) {
      result = BuildCustomError(parser, XML_ERROR_ABORTED,
                                result.bytes_consumed);
      break;
    }
    if (bytes_read == 0)
      break;
  }

  XML_ParserFree(parser);
  return result;
}

XmlParseResult ParseXmlBuffer(const char *xml, size_t size,
                              const XmlParserOptions &options) {
  if (!xml && size != 0) {
    XmlParseResult result;
    result.ok = false;
    result.error_code = XML_ERROR_INVALID_ARGUMENT;
    return result;
  }

  XML_Parser parser = CreateParser(options);
  if (!parser)
    return BuildParseError(NULL, 0);

  XmlParseResult result;
  enum XML_Status status = XML_Parse(parser, xml ? xml : "", (int)size, 1);
  result.bytes_consumed = size;
  if (status == XML_STATUS_ERROR)
    result = BuildParseError(parser, size);

  XML_ParserFree(parser);
  return result;
}

XmlParseResult ParseXmlString(const std::string &xml,
                              const XmlParserOptions &options) {
  return ParseXmlBuffer(xml.c_str(), xml.size(), options);
}

XmlParseResult ParseXmlZipEntry(unzFile uf, const XmlParserOptions &options,
                                size_t chunk_size) {
  return ParseXmlZipEntryTransformed(uf, options, chunk_size, NULL, NULL);
}

XmlParseResult ParseXmlZipEntryTransformed(unzFile uf,
                                           const XmlParserOptions &options,
                                           size_t chunk_size,
                                           XmlTransformChunkFn transform_chunk,
                                           void *transform_ctx) {
  if (!uf || chunk_size == 0) {
    XmlParseResult result;
    result.ok = false;
    result.error_code = XML_ERROR_INVALID_ARGUMENT;
    return result;
  }

  XML_Parser parser = CreateParser(options);
  if (!parser)
    return BuildParseError(NULL, 0);

  static std::vector<char> buffer;
  if (buffer.size() < chunk_size)
    buffer.resize(chunk_size);
  static std::string transformed;
  XmlParseResult result;
  while (true) {
    if (ShouldAbortParse(options)) {
      result = BuildCustomError(parser, XML_ERROR_ABORTED,
                                result.bytes_consumed);
      break;
    }
    int len = unzReadCurrentFile(uf, buffer.data(), (unsigned)buffer.size());
    if (len < 0) {
      result = BuildCustomError(parser, XML_ERROR_EXTERNAL_ENTITY_HANDLING,
                                result.bytes_consumed);
      break;
    }

    const bool final = (len == 0);
    const char *parse_bytes = buffer.data();
    int parse_len = len;
    if (transform_chunk) {
      transformed.clear();
      transform_chunk(std::string(buffer.data(), (size_t)std::max(len, 0)),
                      final, transform_ctx, &transformed);
      parse_bytes = transformed.c_str();
      parse_len = (int)transformed.size();
    }

    enum XML_Status status = XML_Parse(parser, parse_bytes, parse_len, final);
    result.bytes_consumed += (size_t)len;
    if (status == XML_STATUS_ERROR) {
      result = BuildParseError(parser, result.bytes_consumed);
      break;
    }
    if (ShouldAbortParse(options)) {
      result = BuildCustomError(parser, XML_ERROR_ABORTED,
                                result.bytes_consumed);
      break;
    }
    if (len == 0)
      break;
  }

  XML_ParserFree(parser);
  return result;
}

std::string FormatXmlParseError(const XmlParseResult &result) {
  char buf[192];
  snprintf(buf, sizeof(buf), "%d:%d: %s", result.error_line,
           result.error_column, XML_ErrorString(result.error_code));
  return std::string(buf);
}

} // namespace xml_parse_utils
