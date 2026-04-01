#include "formats/txt/txt_loader.h"

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

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestPlainUtf8() {
  std::string path = "/tmp/test_txt_plain.txt";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "Hello world\n");
  fclose(f);

  std::string out;
  ExpectTrue("plain utf8", txt_loader::ReadAndNormalize(path.c_str(), &out));
  ExpectEq("plain content", out, "Hello world\n");
}

void TestMixedNewlines() {
  std::string path = "/tmp/test_txt_newlines.txt";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "Line1\r\nLine2\rLine3\nLine4");
  fclose(f);

  std::string out;
  ExpectTrue("mixed newlines", txt_loader::ReadAndNormalize(path.c_str(), &out));
  ExpectEq("normalized to \\n", out, "Line1\nLine2\nLine3\nLine4");
}

void TestCp1252ToUtf8() {
  std::string path = "/tmp/test_txt_cp1252.txt";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "Espa%csia", 0xf1);
  fclose(f);

  std::string out;
  ExpectTrue("cp1252", txt_loader::ReadAndNormalize(path.c_str(), &out));
  ExpectEq("cp1252 decoded", out, "Espa\xc3\xb1" "sia");
}

void TestEmptyFile() {
  std::string path = "/tmp/test_txt_empty.txt";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fclose(f);

  std::string out;
  ExpectFalse("empty file", txt_loader::ReadAndNormalize(path.c_str(), &out));
}

void TestNonexistentFile() {
  std::string out;
  ExpectFalse("nonexistent", txt_loader::ReadAndNormalize("/tmp/nonexistent_xyz.txt", &out));
}

void TestUtf8WithAccents() {
  std::string path = "/tmp/test_txt_accents.txt";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    Fail("cannot create temp file");
  fprintf(f, "Caf\xc3\xa9 y m\xc3\xbaxima");
  fclose(f);

  std::string out;
  ExpectTrue("utf8 accents", txt_loader::ReadAndNormalize(path.c_str(), &out));
  ExpectEq("accents preserved", out, "Caf\xc3\xa9 y m\xc3\xbaxima");
}

} // namespace

int main() {
  TestPlainUtf8();
  TestMixedNewlines();
  TestCp1252ToUtf8();
  TestEmptyFile();
  TestNonexistentFile();
  TestUtf8WithAccents();
  return 0;
}
