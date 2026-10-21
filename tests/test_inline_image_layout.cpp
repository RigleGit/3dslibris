#include "book/inline_image_layout.h"

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
  req.next_screen_height = 320;
  req.margin_left = 12;
  req.margin_right = 12;
  req.margin_top = 10;
  req.margin_bottom = 36;
  req.next_margin_bottom = 16;
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
        PlanInlineImageLayout(BaseRequest(), Metadata(12, 12));
    ApplyFloatImageLayoutOverride(&plan, true, BaseRequest().linespacing);
    ExpectMode("floated small image is promoted to band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("float band starts on its own line", plan.line_break_before);
    ExpectFalse("float band does not advance screens", plan.advance_before);
    ExpectEq("float band consumes image height", plan.vertical_space_after_draw,
             plan.draw_height + BaseRequest().linespacing);
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
    InlineImageLayoutRequest req = BaseRequest();
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.author_max_width_px = 81;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1920, 760));
    ExpectMode("author-sized leading image remains block band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("author-sized image keeps requested width", plan.draw_width, 81);
    ExpectEq("author-sized image keeps proportional height", plan.draw_height, 32);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.author_max_width_px = 20;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(20, 3));
    ExpectMode("tiny author-sized leading separator uses band layout", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("tiny separator keeps natural height",
             plan.draw_height, 3);
    ExpectEq("tiny separator consumes drawn vertical space",
             plan.vertical_space_after_draw, 3 + req.linespacing);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.author_max_width_px = 22;
    req.line_began = false;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1200, 200));
    ExpectMode("wide author-sized leading ornament uses band layout", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("wide ornament keeps requested width", plan.draw_width, 22);
    ExpectEq("wide ornament consumes drawn vertical space",
             plan.vertical_space_after_draw, plan.draw_height + req.linespacing);
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
    req.line_began = false;
    req.pen_x = req.margin_left;
    req.pen_y = req.margin_top + req.line_height;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1196, 1800));
    ExpectMode("cover-like image at screen start stays page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectFalse("fresh cover-like page image does not pre-advance",
                plan.advance_before);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.line_began = false;
    req.pen_x = req.margin_left;
    req.pen_y = req.margin_top + req.line_height + 2;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1239, 1917));
    ExpectMode("cover-like image near screen start stays page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectFalse("near-start cover-like page image does not pre-advance",
                plan.advance_before);
    ExpectEq("near-start cover keeps next text on right screen",
             plan.next_text_screen, 1);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.screen_width = 400;
    req.screen_height = 240;
    req.next_screen_height = 240;
    req.margin_bottom = 0;
    req.next_margin_bottom = 22;
    req.line_began = false;
    req.pen_x = req.margin_left;
    req.pen_y = req.margin_top + req.line_height;
    req.image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1200, 1574));
    ExpectMode("landscape EPUB cover uses page layout", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectFalse("landscape EPUB cover stays on top screen",
                plan.advance_before);
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
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    req.pen_x = req.margin_left;
    req.pen_y = 220;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1920, 1231));
    ExpectMode("wide leading paragraph image remains band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectFalse("wide landscape band should not force pre-advance",
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
    req.next_screen_height = 400;
    req.margin_bottom = 16;
    req.next_margin_bottom = 36;
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
    req.next_screen_height = 400;
    req.margin_bottom = 16;
    req.next_margin_bottom = 36;
    req.pen_x = 80;
    req.pen_y = 150;
    req.line_began = true;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(800, 1200));
    ExpectMode("page image on right screen mid-page", plan,
               INLINE_IMAGE_LAYOUT_PAGE);
    ExpectFalse("page mid-right does not advance before", plan.advance_before);
    ExpectEq("page image on right sends text to next page left",
             plan.next_text_screen, 0);
    ExpectEq("page image on right crosses one page boundary",
             plan.page_breaks, 1);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.next_screen_height = 400;
    req.margin_bottom = 16;
    req.next_margin_bottom = 36;
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

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.next_screen_height = 400;
    req.margin_bottom = 20;
    req.next_margin_bottom = 36;
    req.pen_x = 12;
    req.pen_y = 54;
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1143, 627));
    ExpectMode("Feature Engineering figure fits on right screen", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectFalse("right-screen figure does not pre-advance", plan.advance_before);
    ExpectEq("right-screen figure keeps text on right", plan.next_text_screen, 1);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.next_screen_height = 400;
    req.margin_bottom = 20;
    req.next_margin_bottom = 36;
    req.pen_x = 12;
    req.pen_y = 286;
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
    InlineImageLayoutPlan plan =
        PlanInlineImageLayout(req, Metadata(1092, 492));
    ExpectMode("late right-screen figure stays band", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("late right-screen figure advances with caption",
               plan.advance_before);
    ExpectEq("late right-screen figure crosses page", plan.page_breaks, 1);
    ExpectEq("late right-screen figure lands on left", plan.next_text_screen, 0);
  }

  {
    InlineImageLayoutRequest req = BaseRequest();
    req.current_screen = 1;
    req.screen_height = 320;
    req.next_screen_height = 400;
    req.margin_bottom = 20;
    req.next_margin_bottom = 36;
    req.pen_x = req.margin_left;
    req.pen_y = 286;
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    InlineImageLayoutPlan plan =
      PlanInlineImageLayout(req, Metadata(1800, 500));
    ExpectMode("leading wide image stays band on compact screen", plan,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectTrue("leading wide image is shrunk below full text width",
               plan.draw_width < (req.screen_width - req.margin_left - req.margin_right));
  }

  // Large figures must not become postage stamps just to keep a caption on
  // a nearly full screen. These dimensions match the reported chart resources.
  for (int screen = 0; screen < 2; ++screen) {
    for (bool landscape : {false, true}) {
      InlineImageLayoutRequest req = BaseRequest();
      req.current_screen = screen;
      req.screen_width = landscape ? (screen == 0 ? 400 : 320) : 240;
      req.screen_height = landscape ? 240 : (screen == 0 ? 400 : 320);
      req.next_screen_height = landscape ? 240 : (screen == 0 ? 320 : 400);
      req.next_screen_width = landscape ? (screen == 0 ? 320 : 400) : 240;
      req.margin_bottom =
          landscape ? (screen == 0 ? 0 : 22) : (screen == 0 ? 36 : 20);
      req.next_margin_bottom =
          landscape ? (screen == 0 ? 22 : 0) : (screen == 0 ? 20 : 36);
      req.pen_x = req.margin_left;
      req.pen_y =
          landscape ? (screen == 0 ? 140 : 160) : (screen == 0 ? 260 : 235);
      req.line_began = false;
      req.image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
      const InlineImageMetadata meta =
          screen == 0 ? Metadata(1085, 737) : Metadata(552, 439);
      const auto plan = PlanInlineImageLayout(req, meta);
      ExpectMode("late chart remains a band", plan, INLINE_IMAGE_LAYOUT_BAND);
      ExpectTrue("late chart moves instead of shrinking to a stamp",
                 plan.advance_before);
      ExpectTrue("late chart keeps readable height", plan.draw_height >= 60);
      ExpectEq("late chart destination", plan.next_text_screen, 1 - screen);
      ExpectEq("late chart page boundary", plan.page_breaks, screen);
      // Planning after the move must produce the same dimensions/consumption,
      // including the 400px -> 320px landscape screen transition.
      req.current_screen = 1 - screen;
      req.screen_width = req.next_screen_width;
      req.screen_height = req.next_screen_height;
      req.margin_bottom = req.next_margin_bottom;
      req.pen_y = req.margin_top + req.line_height;
      const auto fresh = PlanInlineImageLayout(req, meta);
      ExpectEq("moved chart width matches fresh layout", plan.draw_width,
               fresh.draw_width);
      ExpectEq("moved chart height matches fresh layout", plan.draw_height,
               fresh.draw_height);
      ExpectFalse("fresh chart does not advance twice", fresh.advance_before);
    }
  }

  // Advancing must preserve the destination's own image classification and
  // consumed space, including PAGE heuristics and small block-height limits.
  for (bool portrait : {false, true}) {
    auto req = BaseRequest();
    req.current_screen = portrait ? 1 : 0;
    req.screen_width = portrait ? 240 : 400;
    req.screen_height = portrait ? 320 : 240;
    req.next_screen_width = portrait ? 240 : 320;
    req.next_screen_height = portrait ? 400 : 240;
    req.margin_bottom = portrait ? 16 : 0;
    req.next_margin_bottom = portrait ? 36 : 22;
    req.pen_x = req.margin_left;
    req.pen_y = portrait ? 250 : 200;
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
    const auto meta = portrait ? Metadata(800, 1200) : Metadata(80, 90);
    const auto moved = PlanInlineImageLayout(req, meta);
    ExpectTrue("figure advances before fresh classification",
               moved.advance_before);
    req.current_screen = 1 - req.current_screen;
    req.screen_width = req.next_screen_width;
    req.screen_height = req.next_screen_height;
    req.margin_bottom = req.next_margin_bottom;
    req.pen_y = req.margin_top + req.line_height;
    const auto fresh = PlanInlineImageLayout(req, meta);
    ExpectMode("advanced figure keeps destination mode", moved, fresh.mode);
    ExpectEq("advanced figure keeps destination width", moved.draw_width,
             fresh.draw_width);
    ExpectEq("advanced figure keeps destination height", moved.draw_height,
             fresh.draw_height);
    ExpectEq("advanced figure keeps destination consumption",
             moved.vertical_space_after_draw, fresh.vertical_space_after_draw);
    ExpectEq("advanced figure keeps next text screen", moved.next_text_screen,
             fresh.next_text_screen);
    ExpectEq("advanced figure accounts for page boundary", moved.page_breaks,
             fresh.page_breaks + (portrait ? 1 : 0));
    ExpectFalse("fresh destination doesn't advance again",
                fresh.advance_before);
  }

  // A band moved between native screens must consume the destination size.
  for (int screen = 0; screen < 2; ++screen) {
    auto req = BaseRequest();
    req.current_screen = screen;
    req.screen_width = screen == 0 ? 400 : 320;
    req.screen_height = req.next_screen_height = 240;
    req.next_screen_width = screen == 0 ? 320 : 400;
    req.margin_bottom = screen == 0 ? 0 : 22;
    req.next_margin_bottom = screen == 0 ? 22 : 0;
    req.pen_y = 200;
    req.line_began = false;
    const auto moved = PlanInlineImageLayout(req, Metadata(600, 100));
    ExpectTrue("late ordinary band advances", moved.advance_before);
    ExpectMode("late ordinary band remains a band", moved,
               INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("moved ordinary band fits destination content width",
             moved.draw_width, req.next_screen_width - 24);
    // 600:100 aspect ratio, rounded upward by the wide-band planner.
    const int destination_height = screen == 0 ? 50 : 63;
    ExpectEq("moved ordinary band preserves destination aspect ratio",
             moved.draw_height, destination_height);
    ExpectEq("moved ordinary band consumes destination height",
             moved.vertical_space_after_draw, destination_height + 1);
  }

  {
    auto req = BaseRequest();
    req.line_height = 32;
    req.pen_y = 200;
    req.line_began = false;
    req.image_context = INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
    const auto moved = PlanInlineImageLayout(req, Metadata(160, 160));
    ExpectTrue("ordinary block moves to keep following text", moved.advance_before);
    ExpectMode("moved ordinary block stays a band", moved, INLINE_IMAGE_LAYOUT_BAND);
    ExpectEq("moved block uses compact-screen height cap", moved.draw_height, 106);
    ExpectEq("moved block consumes compact-screen space",
             moved.vertical_space_after_draw, 107);
  }

  return 0;
}
