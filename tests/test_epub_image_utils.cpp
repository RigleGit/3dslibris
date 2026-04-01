#include "formats/common/epub_image_utils.h"
#include "string_utils.h"

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

  return 0;
}
