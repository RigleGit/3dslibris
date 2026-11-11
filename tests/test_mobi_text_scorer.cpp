#include "formats/mobi/mobi_text_decode.h"
#include "formats/common/text_helpers.h"
#include "book/book.h"
#include "test_assert.h"

#include <string>
#include <vector>

void Book::SetTitle(const char *) {}

namespace {
unsigned g_seed = 12345;
unsigned Rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 8) & 0xffffff;
}

std::string RandomText(unsigned kind, size_t size) {
  static const char *const pieces[] = {
      "abc ", "the ", "\xc3\xa9",       "\xe2\x82\xac",  "\xf0\x9f\x98\x80",
      "\x93",  "\xe9",  "\xc3\x83\xc2\xa9", "\xc3\xa2\xe2\x82\xac\xe2\x80\x9c",
      "\xc3", "\xe2\x82",   "\x80",           "\n",           "\xff", "<p>"};
  std::string out;
  while (out.size() < size) {
    if (kind == 0) { // mostly ASCII, rare bytes
      out += (Rand() % 40) ? "x" : pieces[Rand() % 15];
    } else if (kind == 1) { // valid UTF-8 with mojibake
      out += pieces[Rand() % 5 + (Rand() % 6 ? 0 : 7)];
    } else { // arbitrary bytes
      out += (char)(Rand() & 0xff);
    }
  }
  return out;
}

std::string Utf16Text(size_t words, bool little, bool bom) {
  std::string out;
  auto put = [&](unsigned w) {
    out += (char)(little ? (w & 0xff) : (w >> 8));
    out += (char)(little ? (w >> 8) : (w & 0xff));
  };
  if (bom)
    put(0xfeff);
  for (size_t i = 0; i < words; ++i) {
    const unsigned r = Rand() % 12;
    if (r == 0) {
      put(0xd83d + (Rand() % 3 == 0 ? 0 : 0));
      put(0xde00 + Rand() % 0x40);
    } else if (r == 1)
      put(0xd800 + Rand() % 0x400); // lone high surrogate
    else
      put(0x20 + Rand() % 0x2000);
  }
  return out;
}

std::vector<size_t> Splits(size_t size) {
  std::vector<size_t> cuts;
  size_t at = 0;
  const unsigned mode = Rand() % 3;
  while (at < size) {
    size_t step = mode == 0 ? 1 : mode == 1 ? 1 + Rand() % 7 : 1 + Rand() % 600;
    at = at + step > size ? size : at + step;
    cuts.push_back(at);
  }
  return cuts;
}

void CheckOne(const std::string &bytes, unsigned encoding) {
  // Whole-buffer reference: the Session.
  std::string expected;
  mobi_text_decode::Session session;
  test::ExpectTrue("session", session.Begin(&bytes, encoding, &expected));
  unsigned guard = 0;
  while (!session.Step(1 + Rand() % 700))
    test::ExpectTrue("session finishes", ++guard < 1000000);

  const std::vector<size_t> cuts = Splits(bytes.size());
  mobi_text_decode::DecodeMode mode = mobi_text_decode::Utf16;
  mobi_text_decode::ModeChoice choice = {mobi_text_decode::Legacy, false, false};
  if (!mobi_text_decode::IsUtf16Encoding(encoding)) {
    mobi_text_decode::Scorer scorer;
    size_t at = 0;
    for (size_t i = 0; i < cuts.size(); ++i) {
      scorer.Feed(bytes.data() + at, cuts[i] - at, i + 1 == cuts.size());
      at = cuts[i];
    }
    if (bytes.empty())
      scorer.Feed("", 0, true);
    const mobi_text_decode::ScanScore &score = scorer.Score();
    test::ExpectEqU("scored bytes", (unsigned)score.bytes,
                    (unsigned)bytes.size());
    test::ExpectEqU("legacy size", (unsigned)score.legacy_size,
                    (unsigned)DecodeLegacySingleByteToUtf8(bytes).size());
    test::ExpectEqU(
        "mixed size", (unsigned)score.mixed_size,
        (unsigned)DecodeMostlyUtf8WithCp1252Fallback(bytes, NULL).size());
    choice = mobi_text_decode::ChooseMode(encoding, score);
    mode = choice.mode;
    test::ExpectTrue("guess flag",
                     choice.guessed == session.UsedUtf8Guess());
    test::ExpectTrue("legacy flag",
                     choice.legacy_guess == session.UsedLegacyGuess());
  }
  mobi_text_decode::Converter conv(mode);
  std::string got;
  size_t at = 0;
  const std::vector<size_t> cuts2 = Splits(bytes.size());
  for (size_t i = 0; i < cuts2.size(); ++i) {
    conv.Feed(bytes.data() + at, cuts2[i] - at, i + 1 == cuts2.size(), &got);
    at = cuts2[i];
  }
  if (bytes.empty())
    conv.Feed("", 0, true, &got);
  test::ExpectTrue("converted bytes equal the session output", got == expected);
}
} // namespace

int main() {
  const unsigned encodings[] = {1252, 65001, 0, 28591};
  for (unsigned round = 0; round < 400; ++round)
    for (unsigned kind = 0; kind < 3; ++kind)
      CheckOne(RandomText(kind, Rand() % 3000), encodings[Rand() % 4]);
  for (unsigned round = 0; round < 300; ++round)
    CheckOne(Utf16Text(Rand() % 700, Rand() % 2, Rand() % 2), 1200);
  CheckOne("", 1252);
  CheckOne("", 1200);
  CheckOne("a", 1200);
  CheckOne(std::string("\xff\xfe", 2), 1200);
  CheckOne(std::string("\xff\xfe\x3d\xd8", 4), 1200);
  CheckOne(std::string("\xff\xfe\x3d\xd8\x00", 5), 1200);
  CheckOne(std::string("\xc3", 1), 65001);
  return 0;
}
