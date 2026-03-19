#pragma once

enum InlineImageLayoutMode {
  INLINE_IMAGE_LAYOUT_INLINE = 0,
  INLINE_IMAGE_LAYOUT_BAND,
  INLINE_IMAGE_LAYOUT_PAGE
};

enum InlineImageContext {
  INLINE_IMAGE_CONTEXT_DEFAULT = 0,
  INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH,
  INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION
};

struct InlineImageMetadata {
  int width;
  int height;
  bool ok;
};

struct InlineImageLayoutRequest {
  int screen_width;
  int screen_height;
  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;
  int line_height;
  int linespacing;
  int pen_x;
  int pen_y;
  bool line_began;
  InlineImageContext image_context;
  int current_screen;
  int follow_text_lines;
};

struct InlineImageLayoutPlan {
  InlineImageLayoutMode mode;
  int draw_width;
  int draw_height;
  bool line_break_before;
  bool advance_before;
  bool consume_rest_of_screen;
  int vertical_space_after_draw;
  int next_text_screen;
  int page_breaks;
};

InlineImageLayoutPlan PlanInlineImageLayout(const InlineImageLayoutRequest &req,
                                            const InlineImageMetadata &meta);
