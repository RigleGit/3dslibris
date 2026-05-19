#include "reader/fixed_layout_input_utils.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

[[noreturn]] void Fail(const std::string &message) {
  std::fprintf(stderr, "%s\n", message.c_str());
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

void ExpectHas(const char *label, uint32_t mask, uint32_t bit) {
  if ((mask & bit) != bit)
    Fail(std::string(label) + ": expected bit in mask");
}

void ExpectMissing(const char *label, uint32_t mask, uint32_t bit) {
  if ((mask & bit) != 0)
    Fail(std::string(label) + ": expected bit absent from mask");
}

} // namespace

int main() {
  const uint32_t key_a = 1u << 0;
  const uint32_t key_b = 1u << 1;
  const uint32_t key_l = 1u << 2;
  const uint32_t key_r = 1u << 3;
  const uint32_t key_zl = 1u << 4;
  const uint32_t key_zr = 1u << 5;
  const uint32_t key_cpad_up = 1u << 6;
  const uint32_t key_cpad_down = 1u << 7;
  const uint32_t key_cpad_left = 1u << 8;
  const uint32_t key_cpad_right = 1u << 9;
  const uint32_t key_dpad_up = 1u << 10;
  const uint32_t key_dpad_down = 1u << 11;
  const uint32_t key_dpad_left = 1u << 12;
  const uint32_t key_dpad_right = 1u << 13;

  ExpectTrue("pdf supports shoulder page turns",
             reader_input_utils::FixedLayoutSupportsShoulderPageTurn(
                 FORMAT_PDF));
  ExpectTrue("cbz supports shoulder page turns",
             reader_input_utils::FixedLayoutSupportsShoulderPageTurn(
                 FORMAT_CBZ));
  ExpectFalse("epub does not use fixed-layout shoulder mapping",
              reader_input_utils::FixedLayoutSupportsShoulderPageTurn(
                  FORMAT_EPUB));

  const uint32_t next_page = reader_input_utils::ReflowableNextPageKeys(
      key_a, key_r, key_cpad_down, key_dpad_down, key_zl);
  ExpectHas("reflowable next keeps face A", next_page, key_a);
  ExpectHas("reflowable next keeps Circle Pad down", next_page,
            key_cpad_down);
  ExpectHas("reflowable next restores D-pad down", next_page, key_dpad_down);
  ExpectMissing("reflowable next does not capture D-pad up", next_page,
                key_dpad_up);

  const uint32_t prev_page = reader_input_utils::ReflowablePrevPageKeys(
      key_b, key_l, key_cpad_up, key_dpad_up, key_zr);
  ExpectHas("reflowable prev keeps face B", prev_page, key_b);
  ExpectHas("reflowable prev keeps Circle Pad up", prev_page, key_cpad_up);
  ExpectHas("reflowable prev restores D-pad up", prev_page, key_dpad_up);
  ExpectMissing("reflowable prev does not capture D-pad down", prev_page,
                key_dpad_down);

  const uint32_t bookmark_prev =
      reader_input_utils::ReflowableBookmarkPrevKeys(key_cpad_right,
                                                     key_dpad_right);
  const uint32_t bookmark_next =
      reader_input_utils::ReflowableBookmarkNextKeys(key_cpad_left,
                                                     key_dpad_left);
  ExpectHas("bookmark previous accepts Circle Pad right", bookmark_prev,
            key_cpad_right);
  ExpectHas("bookmark previous accepts D-pad right", bookmark_prev,
            key_dpad_right);
  ExpectHas("bookmark next accepts Circle Pad left", bookmark_next,
            key_cpad_left);
  ExpectHas("bookmark next accepts D-pad left", bookmark_next, key_dpad_left);

  const uint32_t repeat_next_with_circle =
      reader_input_utils::ReflowablePageRepeatKeys(
          key_cpad_down, key_dpad_down, true);
  ExpectHas("repeat with circle includes Circle Pad down",
            repeat_next_with_circle, key_cpad_down);
  ExpectHas("repeat with circle includes D-pad down",
            repeat_next_with_circle, key_dpad_down);

  const uint32_t repeat_next_without_circle =
      reader_input_utils::ReflowablePageRepeatKeys(
          key_cpad_down, key_dpad_down, false);
  ExpectMissing("repeat without circle excludes Circle Pad down",
                repeat_next_without_circle, key_cpad_down);
  ExpectHas("repeat without circle keeps D-pad down",
            repeat_next_without_circle, key_dpad_down);
  return 0;
}
