#include "parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestAppendByteAndBytes() {
  parsedata_t p{};
  parse_init(&p);

  ExpectTrue("append one byte", parse_append_page_byte(&p, 'A'));
  ExpectEq("buflen after one byte", p.buflen, 1);
  ExpectFalse("no overflow after one byte", parse_page_buffer_overflowed(&p));

  const char *rest = "BCD";
  ExpectEq("append bytes", parse_append_page_bytes(&p, rest, 3), (size_t)3);
  ExpectEq("buflen after bytes", p.buflen, 4);
  ExpectTrue("content preserved", std::memcmp(p.buf, "ABCD", 4) == 0);
}

void TestOverflowTracking() {
  parsedata_t p{};
  parse_init(&p);

  for (int i = 0; i < PAGEBUFSIZE; i++)
    ExpectTrue("fill byte", parse_append_page_byte(&p, 'x'));

  ExpectEq("buffer filled", p.buflen, PAGEBUFSIZE);
  ExpectFalse("overflow still false when exactly full",
              parse_page_buffer_overflowed(&p));
  ExpectFalse("append past end fails", parse_append_page_byte(&p, 'y'));
  ExpectTrue("overflow flagged", parse_page_buffer_overflowed(&p));
  ExpectEq("overflow bytes counted", p.pagebuf_overflow_bytes, (size_t)1);

  const char *extra = "tail";
  ExpectEq("append bytes when full appends nothing",
           parse_append_page_bytes(&p, extra, 4), (size_t)0);
  ExpectEq("overflow bytes accumulate", p.pagebuf_overflow_bytes, (size_t)5);
}

void TestResetClearsOverflow() {
  parsedata_t p{};
  parse_init(&p);
  p.pagebuf_overflow_bytes = 7;
  p.pagebuf_overflowed = true;
  p.buflen = 12;
  parse_reset_page_buffer(&p);
  ExpectEq("buflen reset", p.buflen, 0);
  ExpectEq("overflow bytes reset", p.pagebuf_overflow_bytes, (size_t)0);
  ExpectFalse("overflow flag reset", parse_page_buffer_overflowed(&p));
}

} // namespace

int main() {
  TestAppendByteAndBytes();
  TestOverflowTracking();
  TestResetClearsOverflow();
  return 0;
}
