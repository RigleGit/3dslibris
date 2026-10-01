#include "shared/bugfix_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

[[noreturn]] void Fail(const std::string &msg) {
  fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected)
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
}

void TestFallbackDirs() {
  std::vector<std::string> dirs;
  GetFallbackFontSearchDirs("sdmc:/custom/font", "sdmc:/3ds/3dslibris/font", &dirs);
  ExpectEq("dir count", (int)dirs.size(), 2);
  ExpectTrue("primary dir", dirs[0] == "sdmc:/custom/font");
  ExpectTrue("romfs dir", dirs[1] == "romfs:/3ds/3dslibris/font");
}

void TestFallbackDirsDefault() {
  std::vector<std::string> dirs;
  GetFallbackFontSearchDirs("", "sdmc:/3ds/3dslibris/font", &dirs);
  ExpectTrue("default font dir", dirs[0] == std::string("sdmc:/3ds/3dslibris/font"));
}

void TestGlyphClip() {
  ExpectTrue("inside content right", GlyphWithinContentRight(200, 224));
  ExpectTrue("clipped at content right", !GlyphWithinContentRight(224, 224));
}


}

int main() {
  TestFallbackDirs();
  TestFallbackDirsDefault();
  TestGlyphClip();
  return 0;
}
