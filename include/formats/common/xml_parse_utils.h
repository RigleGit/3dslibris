/*
    3dslibris - xml_parse_utils.h
    Internal Expat wrapper helpers shared across XML-based formats.

    Summary:
    - Centralizes parser setup and teardown for file/buffer/zip inputs.
    - Provides consistent optional handlers and error extraction.
    - Keeps Expat as an internal detail behind small helper functions.
*/

#pragma once

#include "expat.h"
#include "minizip/unzip.h"

#include <stddef.h>
#include <stdio.h>
#include <string>

namespace xml_parse_utils {

typedef bool (*XmlShouldStopFn)(void *user_data);

struct XmlParserOptions {
  XML_StartElementHandler start_element;
  XML_EndElementHandler end_element;
  XML_CharacterDataHandler character_data;
  XML_DefaultHandler default_handler;
  XML_ProcessingInstructionHandler processing_instruction;
  XML_UnknownEncodingHandler unknown_encoding;
  void *unknown_encoding_data;
  void *user_data;
  XmlShouldStopFn abort_parse;
  void *abort_user_data;

  XmlParserOptions()
      : start_element(NULL), end_element(NULL), character_data(NULL),
        default_handler(NULL), processing_instruction(NULL),
        unknown_encoding(NULL), unknown_encoding_data(NULL), user_data(NULL),
        abort_parse(NULL), abort_user_data(NULL) {}
};

struct XmlParseResult {
  bool ok;
  enum XML_Error error_code;
  int error_line;
  int error_column;
  size_t bytes_consumed;

  XmlParseResult()
      : ok(true), error_code(XML_ERROR_NONE), error_line(0), error_column(0),
        bytes_consumed(0) {}
};

XmlParseResult ParseXmlFileStream(FILE *fp, const XmlParserOptions &options,
                                  size_t chunk_size,
                                  XmlShouldStopFn should_stop = NULL);
XmlParseResult ParseXmlBuffer(const char *xml, size_t size,
                              const XmlParserOptions &options);
XmlParseResult ParseXmlString(const std::string &xml,
                              const XmlParserOptions &options);
XmlParseResult ParseXmlZipEntry(unzFile uf, const XmlParserOptions &options,
                                size_t chunk_size);
std::string FormatXmlParseError(const XmlParseResult &result);

} // namespace xml_parse_utils
