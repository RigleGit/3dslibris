/*
 * Stubs for format parsers not exercised by the integration test.
 * book_parser.cpp references all parsers in its dispatch table, so the
 * linker needs every symbol defined even when we only test TXT/FB2/RTF.
 */
#include "formats/cbz/cbz_parser.h"
#include "formats/epub/epub_parser.h"
#include "formats/mobi/mobi_parser.h"
#include "formats/odt/odt_parser.h"
#include "formats/pdf/pdf_parser.h"

namespace epub_parser {
uint8_t Open(Book *, const std::string &) { return 1; }
uint8_t Index(Book *, const std::string &) { return 1; }
int ExtractCover(Book *, const std::string &) { return -1; }
int ResolveToc(Book *, const std::string &) { return -1; }
} // namespace epub_parser

namespace mobi_parser {
u8 ParseFile(Book *, const char *, const Hooks &) { return 1; }
u8 Parse(Book *, const char *) { return 1; }
int ExtractCover(Book *, const std::string &) { return -1; }
} // namespace mobi_parser

namespace odt_parser {
uint8_t Parse(Book *, const char *) { return 1; }
} // namespace odt_parser

namespace pdf_parser {
uint8_t Parse(Book *, const char *) { return 1; }
uint8_t Index(Book *, const char *) { return 1; }
int ExtractCover(Book *, const std::string &) { return -1; }
} // namespace pdf_parser

namespace cbz_parser {
uint8_t Parse(Book *, const char *) { return 1; }
uint8_t Index(Book *, const char *) { return 1; }
int ExtractCover(Book *, const std::string &) { return -1; }
} // namespace cbz_parser
