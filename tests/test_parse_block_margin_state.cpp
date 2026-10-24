#include "parse.h"

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {

XML_Size XMLCALL XML_GetCurrentLineNumber(XML_Parser) { return 0; }
XML_Size XMLCALL XML_GetCurrentColumnNumber(XML_Parser) { return 0; }
enum XML_Error XMLCALL XML_GetErrorCode(XML_Parser) { return XML_ERROR_NONE; }
const XML_LChar *XMLCALL XML_ErrorString(enum XML_Error) { return ""; }

}

namespace {

[[noreturn]] void Fail(const std::string &message) {
  fprintf(stderr, "%s\n", message.c_str());
  std::exit(1);
}

void ExpectEq(const char *label, int actual, int expected) {
  if (actual != expected) {
    Fail(std::string(label) + ": expected " + std::to_string(expected) +
         ", got " + std::to_string(actual));
  }
}

void TestParserResetDropsPendingSpacing() {
  parsedata_t p{};
  parse_init(&p);
  p.pending_block_break = true;
  p.pending_block_spacing_lf = 2;
  p.pending_block_spacing_from_css = true;
  p.pending_block_spacing_advance_ok = true;
  p.pending_block_spacing_suppress_only = true;
  p.current_screen_has_drawable_content = true;
  p.totalbytes = 1234;

  parse_init(&p);
  ExpectEq("reset block break", p.pending_block_break, false);
  ExpectEq("reset spacing lines", p.pending_block_spacing_lf, 0);
  ExpectEq("reset CSS spacing", p.pending_block_spacing_from_css, false);
  ExpectEq("reset spacing advance", p.pending_block_spacing_advance_ok, false);
  ExpectEq("reset suppress-only spacing", p.pending_block_spacing_suppress_only,
           false);
  ExpectEq("reset drawable content", p.current_screen_has_drawable_content, false);
  ExpectEq("reset input size", p.totalbytes, 0);
}

void TestBlockMarginStateInheritsAndRestores() {
  parsedata_t p{};
  parse_init(&p);

  ExpectEq("initial current left", parse_current_block_margin_left(&p), 0);
  ExpectEq("initial current right", parse_current_block_margin_right(&p), 0);

  parse_push(&p, TAG_BLOCKQUOTE);
  parse_set_current_block_margins(&p, 48, 24);
  ExpectEq("blockquote current left", parse_current_block_margin_left(&p), 48);
  ExpectEq("blockquote current right", parse_current_block_margin_right(&p), 24);
  ExpectEq("blockquote global left", p.block_margin_left, 48);
  ExpectEq("blockquote global right", p.block_margin_right, 24);

  parse_push(&p, TAG_P);
  ExpectEq("paragraph inherits left", parse_current_block_margin_left(&p), 48);
  ExpectEq("paragraph inherits right", parse_current_block_margin_right(&p), 24);

  parse_set_current_block_margins(&p, 64, 32);
  ExpectEq("paragraph updated left", parse_current_block_margin_left(&p), 64);
  ExpectEq("paragraph updated right", parse_current_block_margin_right(&p), 32);

  parse_pop(&p);
  ExpectEq("restore parent left", parse_current_block_margin_left(&p), 48);
  ExpectEq("restore parent right", parse_current_block_margin_right(&p), 24);
  ExpectEq("restore parent global left", p.block_margin_left, 48);
  ExpectEq("restore parent global right", p.block_margin_right, 24);

  parse_pop(&p);
  ExpectEq("restore root left", parse_current_block_margin_left(&p), 0);
  ExpectEq("restore root right", parse_current_block_margin_right(&p), 0);
  ExpectEq("restore root global left", p.block_margin_left, 0);
  ExpectEq("restore root global right", p.block_margin_right, 0);
}

} // namespace

int main() {
  TestParserResetDropsPendingSpacing();
  TestBlockMarginStateInheritsAndRestores();
  return 0;
}
