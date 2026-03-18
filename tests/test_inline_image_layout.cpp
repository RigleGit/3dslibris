#include "inline_image_layout.h"

#include <cstdlib>
#include <string>

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

void ExpectTrue(const char *label, bool value) {
  if (!value)
    Fail(std::string(label) + ": expected true");
}

void ExpectFalse(const char *label, bool value) {
  if (value)
    Fail(std::string(label) + ": expected false");
}

InlineImageLayoutRequest BaseRequest() {
  InlineImageLayoutRequest req{};
  req.screen_width = 240;
  req.screen_height = 400;
  req.margin_left = 12;
  req.margin_right = 12;
  req.margin_top = 10;
  req.margin_bottom = 36;
  req.line_height = 16;
  req.linespacing = 1;
  req.pen_x = 12;
  req.pen_y = 26;
  req.line_began = true;
  req.image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
  req.current_screen = 0;
  return req;
}

InlineImageMetadata Metadata(int width, int height, bool ok = true) {
  InlineImageMetadata meta{};
  meta.width = width;
  meta.height = height;
  meta.ok = ok;
  return meta;
}

void ExpectMode(const char *label, const InlineImageLayoutPlan &plan,
                InlineImageLayoutMode mode) {
  if (plan.mode != mode)
    Fail(std::string(label) + ": unexpected layout mode");
}

} // namespace

int main() {
  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(12, 12));
    ExpectMode("small square becomes inline", plan,
               INLINE_IMAGE_LAYOUT_INLINE);
    ExpectEq("inline height equals line height", plan.draw_height, 16);
    ExpectFalse("inline does not advance before", plan.advance_before);
    ExpectEq("inline consumes no extra vertical space", plan.vertical_space_after_draw, 0);
  }

  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(1000, 100));
    ExpectMode("wide thin image becomes band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("band expands to text width", plan.draw_width, 216);
    ExpectTrue("band starts on its own line", plan.line_break_before);
    ExpectFalse("band does not consume full screen", plan.consume_rest_of_screen);
  }

  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(80, 76));
    ExpectMode("medium icon becomes band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("icon band keeps natural width", plan.draw_width, 80);
    ExpectEq("icon band keeps natural height", plan.draw_height, 76);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(90, 78));
    ExpectMode("slightly larger leading paragraph icon stays band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("leading icon keeps natural width", plan.draw_width, 90);
    ExpectEq("leading icon keeps natural height", plan.draw_height, 78);
  }

  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(573, 438));
    ExpectMode("medium map becomes band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("map band uses full text width", plan.draw_width, 216);
    ExpectEq("map band keeps proportional height", plan.draw_height, 165);
  }

  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(800, 1200));
    ExpectMode("large illustration stays page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectTrue("page mode consumes rest of screen", plan.consume_rest_of_screen);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.pen_x = 200;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(48, 16));
    ExpectMode("inline retries on fresh line", plan,
               INLINE_IMAGE_LAYOUT_INLINE);
    ExpectTrue("inline newline retry is requested", plan.line_break_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.margin_left = 100;
    req.margin_right = 100;
    req.pen_x = 120;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(48, 16));
    ExpectMode("inline downgrades to band when line is too narrow", plan,
               INLINE_IMAGE_LAYOUT_BAND);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.pen_x = 40;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1200, 100));
    ExpectMode("band after paragraph keeps text on same screen", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("band moves to next line when mid-line", plan.line_break_before);
    ExpectEq("band continues on same page", plan.page_breaks, 0);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.pen_x = req.margin_left;
    req.pen_y = req.margin_top + req.line_height + 77;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(80, 76));
    ExpectMode("second band can stack when there is no text between", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectFalse("stacked band does not force extra newline", plan.line_break_before);
    ExpectFalse("stacked band stays on same screen when it fits", plan.advance_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.pen_x = 80;
    req.pen_y = req.margin_top + req.line_height + 20;
    req.line_began = true;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(80, 76));
    ExpectMode("band after text remains a block image", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("band after text starts below the text line", plan.line_break_before);
    ExpectEq("band keeps following text on same screen when there is room",
             plan.next_text_screen, req.current_screen);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.pen_x = 200;
    req.pen_y = 360;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1000, 100));
    ExpectMode("band advances when vertical space is exhausted", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("band may pre-advance to next screen", plan.advance_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.pen_x = req.margin_left;
    req.pen_y = 270;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(140, 120));
    ExpectMode("medium icon band can move forward to keep text below", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("icon band advances before when it would orphan text",
               plan.advance_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_DEFAULT;
    req.pen_x = req.margin_left;
    req.pen_y = req.margin_top + req.line_height;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(573, 438));
    ExpectMode("map band stays in place at fresh screen start", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectFalse("fresh-screen band does not pre-advance", plan.advance_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 0;
    req.pen_x = 30;
    req.pen_y = 120;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(800, 1200));
    ExpectMode("page image still moves following text to next page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectTrue("page image advances before when mid-screen", plan.advance_before);
    ExpectEq("page image on left ends with text on next page", plan.page_breaks,
             1);
    ExpectEq("page image on left ends with next text on left", plan.next_text_screen,
             0);
  }

  {
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(BaseRequest(), Metadata(0, 0, false));
    ExpectMode("metadata failure falls back to page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.pen_x = 12;
    req.pen_y = 26;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(800, 1200));
    ExpectMode("page image on right screen at start", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectFalse("page at screen start does not advance", plan.advance_before);
    ExpectEq("page on right screen sends text to next page left",
             plan.next_text_screen, 0);
    ExpectEq("page on right screen increments page break",
             plan.page_breaks, 1);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.pen_x = 80;
    req.pen_y = 150;
    req.line_began = true;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(800, 1200));
    ExpectMode("page image on right screen mid-page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectTrue("page mid-right advances before", plan.advance_before);
    ExpectEq("advance from right goes to left on next page",
             plan.next_text_screen, 1);
    ExpectEq("advance from right crosses one page boundary",
             plan.page_breaks, 1);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.pen_x = 12;
    req.pen_y = 280;
    req.line_began = true;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1000, 100));
    ExpectMode("band on right screen near bottom", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("band near bottom of right screen advances", plan.advance_before);
    ExpectEq("band advance from right crosses page", plan.page_breaks, 1);
    ExpectEq("band after advance goes to left screen", plan.next_text_screen, 0);
  }

  return 0;
}
