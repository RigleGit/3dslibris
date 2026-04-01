#include "formats/common/rtf_control_word_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected [" + expected + "], got [" + actual +
         "]");
  }
}

void AppendTestCodepoint(std::string *out, uint32_t cp) {
  if (!out)
    return;
  if (cp == 0x2014)
    out->append("<emdash>");
  else if (cp == 0x2013)
    out->append("<endash>");
  else if (cp == 0x2022)
    out->append("<bullet>");
  else if (cp == 0x2018)
    out->append("<lquote>");
  else if (cp == 0x2019)
    out->append("<rquote>");
  else if (cp == 0x201C)
    out->append("<ldblquote>");
  else if (cp == 0x201D)
    out->append("<rdblquote>");
}

void TestAsciiControlWords() {
  std::string out;
  ExpectTrue("par replaced", rtf_control_word_utils::AppendReplacement(
                                  "par", 3, &out, &AppendTestCodepoint));
  ExpectEq("par output", out, "\n");

  out.clear();
  ExpectTrue("line replaced", rtf_control_word_utils::AppendReplacement(
                                   "line", 4, &out, &AppendTestCodepoint));
  ExpectEq("line output", out, "\n");

  out.clear();
  ExpectTrue("tab replaced", rtf_control_word_utils::AppendReplacement(
                                  "tab", 3, &out, &AppendTestCodepoint));
  ExpectEq("tab output", out, "\t");
}

void TestUnicodeControlWords() {
  std::string out;
  ExpectTrue("emdash replaced", rtf_control_word_utils::AppendReplacement(
                                     "emdash", 6, &out,
                                     &AppendTestCodepoint));
  ExpectEq("emdash output", out, "<emdash>");

  out.clear();
  ExpectTrue("quote replaced", rtf_control_word_utils::AppendReplacement(
                                    "ldblquote", 9, &out,
                                    &AppendTestCodepoint));
  ExpectEq("quote output", out, "<ldblquote>");
}

void TestUnknownControlWord() {
  std::string out = "seed";
  ExpectFalse("unknown not replaced", rtf_control_word_utils::AppendReplacement(
                                          "unknown", 7, &out,
                                          &AppendTestCodepoint));
  ExpectEq("unknown leaves output untouched", out, "seed");
}

} // namespace

int main() {
  TestAsciiControlWords();
  TestUnicodeControlWords();
  TestUnknownControlWord();
  return 0;
}
