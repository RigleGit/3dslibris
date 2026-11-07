#include "formats/mobi/mobi_text_decode.h"
#include "book/book.h"
#include <cassert>
#include <string>
void Book::SetTitle(const char *) {}
int main() {
  struct Case {
    std::string bytes, expected;
    unsigned encoding;
    bool guessed;
  };
  const Case cases[] = {
      {"caf\xe9", "caf\xc3\xa9", 1252, false},
      {"caf\xc3\xa9\xc3\xa9", "caf\xc3\xa9\xc3\xa9", 1252, true},
      {"abc\x93", "abc\xe2\x80\x9c", 65001, false},
      {std::string("\xff\xfe\x3d\xd8\x00\xde", 6), "\xf0\x9f\x98\x80", 1200,
       false},
      {std::string(3000, 'a') + "\xc3\xa9\xc3\xa9\x93",
       std::string(3000, 'a') + "\xc3\xa9\xc3\xa9\xe2\x80\x9c", 1252, true}};
  for (const auto &c : cases) {
    for (size_t chunk = 1; chunk <= 9; ++chunk) {
      std::string out;
      mobi_text_decode::Session s;
      assert(s.Begin(&c.bytes, c.encoding, &out));
      unsigned steps = 0;
      while (!s.Step(chunk))
        assert(++steps < 100000);
      assert(out == c.expected && s.UsedUtf8Guess() == c.guessed);
    }
  }
}
