#include "shared/utf8_utils.h"

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
  if (actual != expected)
    Fail(std::string(label) + ": unexpected value");
}

void ExpectEq(const char *label, size_t actual, size_t expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

std::string EspanaUtf8() { return std::string("Espa") + "\xc3\xb1" + "a"; }

void TestValidUtf8() {
  ExpectTrue("valid utf8 string",
             utf8_utils::IsValidUtf8(std::string("coraz\xc3\xb3n")));
  ExpectTrue("valid utf8 c string",
             utf8_utils::IsValidUtf8(EspanaUtf8().c_str()));
  ExpectFalse("invalid utf8 string",
              utf8_utils::IsValidUtf8(std::string("\xc3", 1)));
  ExpectFalse("invalid utf8 c string", utf8_utils::IsValidUtf8("\xc3"));
}

void TestDecodeCp1252ToUtf8() {
  const std::string hola_dash = std::string("Hola ") + "\x97" + " mundo";
  ExpectEq("cp1252 em dash",
           utf8_utils::DecodeCp1252ToUtf8(hola_dash),
           "Hola \xe2\x80\x94 mundo");
  const std::string espana_cp1252 = std::string("Espa") + "\xf1" + "a";
  ExpectEq("cp1252 enye",
           utf8_utils::DecodeCp1252ToUtf8(espana_cp1252),
           EspanaUtf8());
}

void TestRepairMojibakeUtf8() {
  std::string repaired;
  ExpectTrue("repairs mojibake",
             utf8_utils::TryRepairMojibakeUtf8(
                 "coraz\xc3\x83\xc2\xb3n", &repaired));
  ExpectEq("mojibake repaired", repaired, "coraz\xc3\xb3n");
  ExpectFalse("plain utf8 no repair",
              utf8_utils::TryRepairMojibakeUtf8("coraz\xc3\xb3n", &repaired));
}

void TestRepairFullwidthByteMojibake() {
  std::string repaired;
  ExpectTrue("repairs fullwidth bytes",
             utf8_utils::TryRepairFullwidthByteMojibake(
                 "coraz\xef\xbf\x83\xef\xbe\xb3n", &repaired));
  ExpectEq("fullwidth repair", repaired, "coraz\xc3\xb3n");
}

void TestComposeLatinCombiningMarks() {
  ExpectEq("compose acute tilde and diaeresis",
           utf8_utils::ComposeLatinCombiningMarks(
               "a\xcc\x81 n\xcc\x83 U\xcc\x88"),
           "\xc3\xa1 \xc3\xb1 \xc3\x9c");
}

void TestUtf16NameToUtf8() {
  const uint16_t name[] = {'H', 0x00F1, 'o', 0};
  std::string out;
  ExpectTrue("utf16 to utf8", utf8_utils::Utf16NameToUtf8(name, &out));
  ExpectEq("utf16 output", out, "H\xc3\xb1o");
}

void TestNormalizeFsFilenameForIo() {
  ExpectEq("normalize fullwidth filename",
           utf8_utils::NormalizeFsFilenameForIo(
               "coraz\xef\xbf\x83\xef\xbe\xb3n.epub"),
           "coraz\xc3\xb3n.epub");
  ExpectEq("leave utf8 filename",
           utf8_utils::NormalizeFsFilenameForIo("libro\xc3\xb1.epub"),
           "libro\xc3\xb1.epub");
  ExpectEq("compose acute accented filename",
           utf8_utils::NormalizeFsFilenameForIo("cai\xcc\x81" "das.epub"),
           "ca\xc3\xad" "das.epub");
  ExpectEq("compose acute accented title filename",
           utf8_utils::NormalizeFsFilenameForIo("La ilusio\xcc\x81n.epub"),
           "La ilusi\xc3\xb3n.epub");
}

void TestDecodeMostlyUtf8WithCp1252Fallback() {
  size_t invalid = 0;
  const std::string mixed = std::string("caf\xc3\xa9 ") + "\x97";
  ExpectEq("mixed decode",
           utf8_utils::DecodeMostlyUtf8WithCp1252Fallback(mixed, &invalid),
           "caf\xc3\xa9 \xe2\x80\x94");
  ExpectEq("invalid count", invalid, (size_t)1);
  ExpectEq("invalid lead bytes",
           utf8_utils::CountUtf8InvalidLeadBytes(mixed),
           (size_t)1);
}

} // namespace

int main() {
  TestValidUtf8();
  TestDecodeCp1252ToUtf8();
  TestRepairMojibakeUtf8();
  TestRepairFullwidthByteMojibake();
  TestComposeLatinCombiningMarks();
  TestUtf16NameToUtf8();
  TestNormalizeFsFilenameForIo();
  TestDecodeMostlyUtf8WithCp1252Fallback();
  return 0;
}
