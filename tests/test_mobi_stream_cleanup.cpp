#include "formats/mobi/mobi_text_cleanup.h"
#include "test_assert.h"

#include <string>
#include <vector>

namespace {
unsigned g_seed = 99;
unsigned Rand() {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 8) & 0xffffff;
}

void NormalizeNewlines(std::string *s) {
  if (s->find('\r') == std::string::npos)
    return;
  size_t write = 0;
  for (size_t read = 0; read < s->size(); read++) {
    char c = (*s)[read];
    if (c == '\r') {
      if (read + 1 < s->size() && (*s)[read + 1] == '\n')
        read++;
      (*s)[write++] = '\n';
    } else
      (*s)[write++] = c;
  }
  s->resize(write);
}

bool IsCtx(unsigned char c) { return c == 7 || c == 8 || c == 14; }

std::vector<unsigned short> Ids(const std::string &t) {
  std::vector<unsigned short> ids;
  for (size_t i = 0; i < t.size();) {
    const unsigned char c0 = t[i];
    size_t image_pos = i, len = 0;
    if (c0 == 6) {
      if (i + 2 < t.size())
        len = 3;
    } else if (IsCtx(c0) && i + 3 < t.size() && (unsigned char)t[i + 1] == 6) {
      image_pos = i + 1;
      len = 4;
    }
    if (!len) {
      ++i;
      continue;
    }
    ids.push_back((unsigned short)(((unsigned char)t[image_pos + 1] << 8) |
                                   (unsigned char)t[image_pos + 2]));
    i += len;
  }
  return ids;
}

std::string RandomBook(bool with_tokens) {
  static const char *const words[] = {
      "the",   "quick",  "brown",  "fox",    "jumps", "over",  "lazy",
      "Dog",   "CHAPTER", "ONE",   "two",    "caf\xc3\xa9",    "a-",
      "sub-",  "x\xc2\xad", "end.",  "wow!",   "why?",  "1984",  "- item",
      "\xe2\x80\x94", "\xc3\x83\xc2\xa9", "\xc3\x83\xc2\xb1x", "\xc3\x83",
      "\xc3\x83\xc3\xa9", "\xc3\xa1rbol"};
  const size_t count = sizeof(words) / sizeof(words[0]);
  std::string out;
  const unsigned lines = Rand() % 70;
  for (unsigned l = 0; l < lines; ++l) {
    const unsigned n = Rand() % 14;
    for (unsigned w = 0; w < n; ++w) {
      if (with_tokens && Rand() % 25 == 0) {
        const unsigned id = Rand() % 6 == 0 ? 0x0a0a : Rand() % 300;
        if (Rand() % 2)
          out += (char)(Rand() % 2 ? 7 : 14);
        out += (char)6;
        out += (char)(id >> 8);
        out += (char)(id & 0xff);
      }
      out += words[Rand() % count];
      out += Rand() % 9 == 0 ? "  " : Rand() % 11 == 0 ? "\t" : " ";
    }
    switch (Rand() % 8) {
    case 0: out += "\n\n"; break;
    case 1: out += "\n\n\n"; break;
    case 2: out += "\r\n"; break;
    case 3: out += "\r"; break;
    case 4: out += " \n \n"; break;
    default: out += "\n"; break;
    }
  }
  if (Rand() % 4 == 0)
    out = std::string(Rand() % 4, ' ') + out;
  if (Rand() % 4 == 0)
    out += std::string(Rand() % 4, '\n');
  return out;
}

std::string Reference(std::string text, const mobi_text_cleanup::StreamOptions &o) {
  NormalizeNewlines(&text);
  if (o.mojibake) {
    if (o.token_aware)
      text = mobi_text_cleanup::RepairCommonMojibakePreservingMobiImageTokens(text);
    else
      mobi_text_cleanup::RepairCommonMojibakeInPlace(&text);
  }
  if (o.wrap_fix)
    text = o.token_aware
               ? mobi_text_cleanup::FixBrokenParagraphWrapsPreservingMobiImageTokens(text)
               : mobi_text_cleanup::FixBrokenParagraphWraps(text);
  return text;
}

void Check(const std::string &text, const mobi_text_cleanup::StreamOptions &o) {
  const std::string expected = Reference(text, o);
  mobi_text_cleanup::CleanupStream stream(o);
  std::string got;
  const unsigned mode = Rand() % 3;
  size_t at = 0;
  do {
    size_t step = mode == 0 ? 1 : mode == 1 ? 1 + Rand() % 6 : 1 + Rand() % 200;
    if (at + step > text.size())
      step = text.size() - at;
    stream.Feed(text.data() + at, step, at + step == text.size(), &got);
    at += step;
  } while (at < text.size());
  if (got != expected) {
    fprintf(stderr, "opts moj=%d wrap=%d tok=%d\ntext=[", o.mojibake, o.wrap_fix, o.token_aware);
    for (unsigned char c : text) fprintf(stderr, c < 32 || c > 126 ? "\\x%02x" : "%c", c);
    fprintf(stderr, "]\nexp =[");
    for (unsigned char c : expected) fprintf(stderr, c < 32 || c > 126 ? "\\x%02x" : "%c", c);
    fprintf(stderr, "]\ngot =[");
    for (unsigned char c : got) fprintf(stderr, c < 32 || c > 126 ? "\\x%02x" : "%c", c);
    fprintf(stderr, "]\n");
  }
  test::ExpectTrue("stream equals the whole-text cleanup", got == expected);
  if (o.token_aware) {
    std::string normalized = text;
    NormalizeNewlines(&normalized);
    test::ExpectTrue("ids before", stream.IdsBefore() == Ids(normalized));
    test::ExpectTrue("ids after", stream.IdsAfter() == Ids(expected));
  }
}
} // namespace

int main() {
  for (unsigned round = 0; round < 3000; ++round) {
    const bool tokens = Rand() % 2;
    const std::string text = RandomBook(tokens);
    mobi_text_cleanup::StreamOptions o;
    o.mojibake = Rand() % 2;
    o.wrap_fix = Rand() % 2;
    o.token_aware = tokens && (o.mojibake || o.wrap_fix) ? Rand() % 4 != 0 : false;
    Check(text, o);
  }
  mobi_text_cleanup::StreamOptions all;
  all.mojibake = all.wrap_fix = all.token_aware = true;
  Check("", all);
  Check("   \n ", all);
  Check("x", all);
  all.token_aware = false;
  Check("", all);
  Check("   \n ", all);
  Check("\n\nabc def ghi jkl mno pqr\n\nstu vwx yz.", all);

  // Token scan on its own, split anywhere.
  for (unsigned round = 0; round < 500; ++round) {
    const std::string text = RandomBook(true);
    mobi_text_cleanup::TokenScan scan;
    size_t at = 0;
    do {
      size_t step = 1 + Rand() % 9;
      if (at + step > text.size())
        step = text.size() - at;
      scan.Feed(text.data() + at, step, at + step == text.size());
      at += step;
    } while (at < text.size());
    test::ExpectTrue("token scan", scan.Ids() == Ids(text));
  }
  return 0;
}
