#include "formats/rtf/rtf_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  if (actual != expected)
    Fail(std::string(label) + ": expected [" + expected + "], got [" + actual +
         "]");
}

void TestBasicRtf() {
  std::string path = "/tmp/test_rtf_basic.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "{\\rtf1\\ansi Hello World}");
  fclose(f);

  std::string out;
  ExpectTrue("basic rtf", rtf_loader::ReadAndDecode(path.c_str(), &out));
  ExpectEq("basic content", out, "Hello World");
}

void TestRtfWithNewlines() {
  std::string path = "/tmp/test_rtf_newlines.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "{\\rtf1\\ansi Line1\\par Line2}");
  fclose(f);

  std::string out;
  ExpectTrue("rtf newlines", rtf_loader::ReadAndDecode(path.c_str(), &out));
  ExpectEq("normalized newlines", out, "Line1\nLine2");
}

void TestRtfWithUnicode() {
  std::string path = "/tmp/test_rtf_unicode.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "{\\rtf1\\ansi Caf\\'e9}");
  fclose(f);

  std::string out;
  ExpectTrue("rtf unicode", rtf_loader::ReadAndDecode(path.c_str(), &out));
  ExpectEq("unicode decoded", out, "Caf\xc3\xa9");
}

void TestRtfWithParagraph() {
  std::string path = "/tmp/test_rtf_par.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "{\\rtf1\\ansi First\\par\\par Second}");
  fclose(f);

  std::string out;
  ExpectTrue("rtf paragraph", rtf_loader::ReadAndDecode(path.c_str(), &out));
  ExpectEq("paragraph spacing", out, "First\n\nSecond");
}

void TestEmptyFile() {
  std::string path = "/tmp/test_rtf_empty.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fclose(f);

  std::string out;
  ExpectFalse("empty file", rtf_loader::ReadAndDecode(path.c_str(), &out));
}

void TestNonexistentFile() {
  std::string out;
  ExpectFalse("nonexistent", rtf_loader::ReadAndDecode("/tmp/nonexistent_xyz.rtf", &out));
}

void TestRtfOnlyControlWords() {
  std::string path = "/tmp/test_rtf_controls.rtf";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "{\\rtf1\\ansi}");
  fclose(f);

  std::string out;
  ExpectTrue("rtf empty body", rtf_loader::ReadAndDecode(path.c_str(), &out));
  ExpectEq("empty rtf body", out, "");
}

} // namespace

int main() {
  TestBasicRtf();
  TestRtfWithNewlines();
  TestRtfWithUnicode();
  TestRtfWithParagraph();
  TestEmptyFile();
  TestNonexistentFile();
  TestRtfOnlyControlWords();
  return 0;
}
