#include "shared/text_unicode_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

void ExpectEq(const char *label, uint32_t actual, uint32_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestDecodeCp1252Fallback() {
  std::vector<text_unicode_utils::TextCodepoint> run;
  const std::string text = std::string("A") + "\x97" + "B";
  ExpectTrue("cp1252 run decode",
             text_unicode_utils::BuildTextRunUtf8(text.c_str(), text.size(),
                                                  NULL, &run));
  ExpectEq("cp1252 run size", run.size(), (size_t)3);
  ExpectEq("cp1252 em dash codepoint", run[1].codepoint, (uint32_t)0x2014);
  ExpectEq("cp1252 byte length", run[1].byte_length, (size_t)1);
}

void TestGraphemeBoundary() {
  std::vector<text_unicode_utils::TextCodepoint> run;
  const std::string text = std::string("a") + "\xCC\x81" + "b";
  ExpectTrue("grapheme run decode",
             text_unicode_utils::BuildTextRunUtf8(text.c_str(), text.size(),
                                                  NULL, &run));
  ExpectEq("grapheme run size", run.size(), (size_t)3);
  ExpectTrue("base starts grapheme", run[0].grapheme_start);
  ExpectFalse("combining mark stays in grapheme", run[1].grapheme_start);
  ExpectTrue("next base starts grapheme", run[2].grapheme_start);
  ExpectEq("display bytes for first grapheme",
           text_unicode_utils::Utf8BytesForDisplayChars(text.c_str(), 1),
           (size_t)3);
}

void TestLineBreaks() {
  std::vector<text_unicode_utils::TextCodepoint> run;
  ExpectTrue("ascii breaks",
             text_unicode_utils::BuildTextRunUtf8("hola mundo", 10, NULL,
                                                  &run));
  ExpectEq("ascii size", run.size(), (size_t)10);
  ExpectTrue("space is breakable space", run[4].breakable_space);
  ExpectTrue("break allowed after ascii space", run[4].allow_break_after);

  run.clear();
  const std::string nbsp = std::string("hola") + "\xC2\xA0" + "mundo";
  ExpectTrue("nbsp breaks",
             text_unicode_utils::BuildTextRunUtf8(nbsp.c_str(), nbsp.size(),
                                                  NULL, &run));
  ExpectEq("nbsp size", run.size(), (size_t)10);
  ExpectFalse("nbsp is not breakable space", run[4].breakable_space);
  ExpectFalse("no break after nbsp", run[4].allow_break_after);
}

void TestListMarkerNormalization() {
  const std::string pua_bullet = std::string("\xEF\x82\xB7") + " texto";
  ExpectEq("strip pua bullet bytes",
           text_unicode_utils::StripLeadingListMarkerUtf8(pua_bullet.c_str()),
           (size_t)4);

  const std::string standard_bullet = std::string("\xE2\x80\xA2") + " texto";
  ExpectEq("strip unicode bullet bytes",
           text_unicode_utils::StripLeadingListMarkerUtf8(
               standard_bullet.c_str()),
           (size_t)4);

  ExpectEq("do not strip plain text",
           text_unicode_utils::StripLeadingListMarkerUtf8("texto"), (size_t)0);
}

} // namespace

int main() {
  TestDecodeCp1252Fallback();
  TestGraphemeBoundary();
  TestLineBreaks();
  TestListMarkerNormalization();
  return 0;
}
