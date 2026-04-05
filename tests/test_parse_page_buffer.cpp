#include "parse.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

bool MockFlushPage(parsedata_t *p, void *ctx) {
  if (ctx)
    (*(int *)ctx)++;
  parse_reset_page_buffer(p);
  return true;
}

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

  const u32 rest[] = {'B', 'C', 'D'};
  ExpectEq("append bytes", parse_append_page_bytes(&p, rest, 3), (size_t)3);
  ExpectEq("buflen after bytes", p.buflen, 4);
  ExpectTrue("content preserved",
             p.buf[0] == 'A' && p.buf[1] == 'B' && p.buf[2] == 'C' &&
                 p.buf[3] == 'D');
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

  const u32 extra[] = {'t', 'a', 'i', 'l'};
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

void TestSoftBreakForSmallPendingWrite() {
  parsedata_t p{};
  parse_init(&p);

  for (int i = 0; i < PAGEBUFSIZE; i++)
    ExpectTrue("fill full page", parse_append_page_byte(&p, 'x'));

  int flush_count = 0;
  const u32 tail[] = {'Y', 'Z'};
  ExpectEq("soft append writes full tail",
           parse_append_page_bytes_soft(&p, tail, 2, MockFlushPage,
                                        &flush_count),
           (size_t)2);
  ExpectEq("flush called once", flush_count, 1);
  ExpectEq("buflen after soft break", p.buflen, 2);
  ExpectTrue("tail copied after flush",
             p.buf[0] == 'Y' && p.buf[1] == 'Z');
  ExpectFalse("overflow cleared on soft break", parse_page_buffer_overflowed(&p));
}

void TestSoftBreakDoesNotSplitOversizedWrite() {
  parsedata_t p{};
  parse_init(&p);

  for (int i = 0; i < PAGEBUFSIZE; i++)
    ExpectTrue("fill full page oversized", parse_append_page_byte(&p, 'x'));

  int flush_count = 0;
  std::vector<u32> huge(PAGEBUFSIZE + 1, (u32)'z');
  ExpectEq("oversized write remains capped",
           parse_append_page_bytes_soft(&p, huge.data(), huge.size(),
                                        MockFlushPage, &flush_count),
           (size_t)0);
  ExpectEq("no flush for oversized write", flush_count, 0);
  ExpectTrue("overflow set for oversized write", parse_page_buffer_overflowed(&p));
}

void TestParseInitDisablesPreformattedWrapByDefault() {
  parsedata_t p{};
  parse_init(&p);
  ExpectFalse("preformatted wrap disabled by default",
              p.preformatted_wrap_enabled);
}

} // namespace

int main() {
  TestAppendByteAndBytes();
  TestOverflowTracking();
  TestResetClearsOverflow();
  TestSoftBreakForSmallPendingWrite();
  TestSoftBreakDoesNotSplitOversizedWrite();
  TestParseInitDisablesPreformattedWrapByDefault();
  return 0;
}
