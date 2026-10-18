#include "formats/common/epub_image_utils.h"
#include "shared/base64_utils.h"
#include "shared/string_utils.h"

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

void ExpectEq(const char *label, const std::string &actual,
              const std::string &expected) {
  if (actual != expected)
    Fail(std::string(label) + ": unexpected string");
}

} // namespace

int main() {
  using epub_image_utils::DecodeDataUriImage;
  using epub_image_utils::LooksLikeSvgWrapper;

  ExpectTrue("startswith no case", StartsWithNoCase("DaTa:image/png", "data:"));
  ExpectFalse("startswith mismatch", StartsWithNoCase("file.png", "data:"));

  ExpectTrue("svg extension counts as wrapper",
             LooksLikeSvgWrapper("images/cover.SVG",
                                 std::vector<unsigned char>()));
  ExpectFalse("raster extension without probe data is not svg wrapper",
              LooksLikeSvgWrapper("images/cover.jpg",
                                  std::vector<unsigned char>()));
  ExpectTrue("svg content counts as wrapper",
             LooksLikeSvgWrapper("images/cover.bin",
                                 std::vector<unsigned char>(
                                     {'<', 's', 'v', 'g', ' ', 'x'})));
  ExpectFalse("png content is not svg wrapper",
              LooksLikeSvgWrapper("images/cover.png",
                                  std::vector<unsigned char>(
                                      {0x89, 'P', 'N', 'G'})));

  std::vector<unsigned char> out;
  ExpectTrue("decode data uri image",
             DecodeDataUriImage("data:image/png;base64,SGVsbG8=", &out, 64));
  ExpectEq("decoded data uri bytes",
           std::string((const char *)out.data(), out.size()), "Hello");

  out.clear();
  ExpectFalse("reject non-base64 data uri",
              DecodeDataUriImage("data:text/plain,Hello", &out, 64));
  image_memory::Bytes checked;
  ExpectTrue("checked source decodes data URI",
      DecodeDataUriImage("data:image/png;base64,SGVsbG8=",&checked,64));
  ExpectEq("checked source original bytes",
      std::string((const char*)checked.data(),checked.size()),"Hello");
  ExpectFalse("checked source rejects oversized decoded payload",
      DecodeDataUriImage("data:image/png;base64,SGVsbG8=",&checked,4));
  ExpectTrue("failed source leaves no partial bytes",checked.empty());

  struct DecodeCase {
    const char *payload;
    size_t limit;
    bool succeeds;
    std::string expected;
  };
  const DecodeCase cases[] = {
      {"SGVsbG8=", 5, true, "Hello"},
      {" S G\tV s\rb G\n8=", 5, true, "Hello"},
      {"SGVsbG8", 5, true, "Hello"},
      {"-_8=", 2, true, std::string("\xfb\xff", 2)},
      {"SGVsbG8=ignored", 5, true, "Hello"},
      {"SGVsbG8=", 4, false, ""},
      {"AA==", 0, false, ""},
      {"SGVsbG8!", 5, false, ""},
      {"A", 5, false, ""},
      {" \t\r\n", 5, false, ""},
  };
  for (const DecodeCase &entry : cases) {
    out.assign(3, 'x');
    ExpectTrue("legacy Base64 result matches limit and alphabet",
        DecodeBase64Bytes(entry.payload, &out, entry.limit) == entry.succeeds);
    ExpectEq("legacy Base64 returns complete bytes or empty failure",
        out.empty() ? "" : std::string((const char *)out.data(), out.size()), entry.expected);
    const std::string uri = std::string("data:image/png;base64,") + entry.payload;
    ExpectTrue("checked data URI result matches limit and alphabet",
        DecodeDataUriImage(uri, &checked, entry.limit) == entry.succeeds);
    ExpectEq("checked data URI returns complete bytes or empty failure",
        checked.empty() ? "" :
            std::string((const char *)checked.data(), checked.size()), entry.expected);
  }

  return 0;
}
