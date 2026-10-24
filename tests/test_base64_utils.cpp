#include "shared/base64_utils.h"

#include <cassert>
#include <cstdio>

int main() {
  struct Case { const char *encoded; const char *decoded; };
  const Case cases[] = {
      {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"},
      {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
      {"Zm9vYmFy", "foobar"}, {" Zm9v\nYmFy\t", "foobar"}};
  std::vector<u8> bytes;
  for (const Case &test : cases) {
    assert(DecodeBase64Bytes(test.encoded, &bytes, 32));
    assert(std::string(bytes.begin(), bytes.end()) == test.decoded);
  }
  // The accumulator wraps repeatedly while the decoded bytes remain 0..31.
  assert(DecodeBase64Bytes("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=", &bytes, 32));
  assert(bytes.size() == 32);
  for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == (u8)i);
  for (const char *encoded : {"+/8=", "-_8="}) {
    assert(DecodeBase64Bytes(encoded, &bytes, 2));
    assert(bytes.size() == 2 && bytes[0] == 0xfb && bytes[1] == 0xff);
  }
  assert(DecodeBase64Bytes("Zm9vYmFy", &bytes, 6));
  assert(!DecodeBase64Bytes("Zm9vYmFy", &bytes, 5));
  assert(!DecodeBase64Bytes("Zg==", &bytes, 0));
  assert(!DecodeBase64Bytes("Zm9v?YmFy", &bytes, 32));
  assert(!DecodeBase64Bytes("", &bytes, 32));
  assert(!DecodeBase64Bytes("Zg==", nullptr, 32));
  puts("PASS: Base64 vectors, binary bytes, wrapping accumulator and limits");
}
