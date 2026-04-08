#include "book/inline_image_layout.h"

#include <algorithm>

namespace {

static int ClampPositive(int value, int fallback) {
  return (value > 0) ? value : fallback;
}

static int DivRoundNearest(int numer, int denom) {
  if (denom <= 0)
    return 0;
  return (numer + (denom / 2)) / denom;
}

static int DivRoundUp(int numer, int denom) {
  if (denom <= 0)
    return 0;
  return (numer + denom - 1) / denom;
}

static void FitWithinBoxNoUpscale(int src_w, int src_h, int max_w, int max_h,
                                  int *out_w, int *out_h) {
  if (!out_w || !out_h)
    return;
  src_w = ClampPositive(src_w, 1);
  src_h = ClampPositive(src_h, 1);
  max_w = ClampPositive(max_w, src_w);
  max_h = ClampPositive(max_h, src_h);

  int draw_w = src_w;
  int draw_h = src_h;
  if (draw_w > max_w || draw_h > max_h) {
    int scale_x = (max_w * 1024) / draw_w;
    int scale_y = (max_h * 1024) / draw_h;
    int scale = std::min(scale_x, scale_y);
    scale = std::max(1, std::min(scale, 1024));
    draw_w = std::max(1, (draw_w * scale + 512) / 1024);
    draw_h = std::max(1, (draw_h * scale + 512) / 1024);
  }

  *out_w = draw_w;
  *out_h = draw_h;
}

static bool IsAtScreenStart(const InlineImageLayoutRequest &req) {
  return !req.line_began && req.pen_x == req.margin_left &&
         req.pen_y == (req.margin_top + req.line_height);
}

static int NextScreenHeight(const InlineImageLayoutRequest &req,
                            int *page_breaks) {
  const int fallback_height = (req.current_screen == 1) ? 400 : 320;
  const int next_screen_height =
      (req.next_screen_height > 0) ? req.next_screen_height : fallback_height;
  if (req.current_screen == 1) {
    if (page_breaks)
      (*page_breaks)++;
    return next_screen_height;
  }
  return next_screen_height;
}

static void FillPageMode(const InlineImageLayoutRequest &req,
                         InlineImageLayoutPlan *plan) {
  plan->mode = INLINE_IMAGE_LAYOUT_PAGE;
  plan->consume_rest_of_screen = true;
  plan->vertical_space_after_draw = 0;
  plan->line_break_before = false;
  plan->advance_before = !IsAtScreenStart(req);
  plan->page_breaks = 0;

  int image_screen = req.current_screen;
  if (plan->advance_before) {
    if (image_screen == 1) {
      image_screen = 0;
      plan->page_breaks++;
    } else {
      image_screen = 1;
    }
  }

  if (image_screen == 1) {
    plan->next_text_screen = 0;
    plan->page_breaks++;
  } else {
    plan->next_text_screen = 1;
  }
}

} // namespace

InlineImageLayoutPlan PlanInlineImageLayout(const InlineImageLayoutRequest &req,
                                            const InlineImageMetadata &meta) {
  InlineImageLayoutPlan plan{};
  FillPageMode(req, &plan);

  const bool leading_paragraph_image =
      req.image_context == INLINE_IMAGE_CONTEXT_LEADING_PARAGRAPH;
  const bool figure_with_caption =
      req.image_context == INLINE_IMAGE_CONTEXT_FIGURE_WITH_CAPTION;
  const int line_height = ClampPositive(req.line_height, 16);
  const int screen_width = ClampPositive(req.screen_width, 240);
  const int screen_height = ClampPositive(req.screen_height, 400);
  const int text_width =
      std::max(1, screen_width - req.margin_left - req.margin_right);
  const int limit_y = screen_height - req.margin_bottom;
  const int band_max_height =
      std::min(12 * line_height, (screen_height * 3) / 5);
  const int block_band_max_height =
      std::min(8 * line_height, screen_height / 3);
  const int min_follow_lines =
      std::max(2, req.follow_text_lines > 0 ? req.follow_text_lines : 3);
  const int min_caption_lines =
      std::max(2, req.follow_text_lines > 0 ? req.follow_text_lines : 3);
  const int min_follow_text_height =
      min_follow_lines * (line_height + req.linespacing);
  const int min_caption_text_height =
      min_caption_lines * (line_height + req.linespacing);
  const int next_margin_bottom =
      (req.next_margin_bottom >= 0) ? req.next_margin_bottom : req.margin_bottom;

  if (!meta.ok || meta.width <= 0 || meta.height <= 0)
    return plan;

  const int inline_height = line_height;
  const int inline_width =
      std::max(1, DivRoundNearest(meta.width * inline_height, meta.height));
  // Inline is reserved for genuinely small ornament-like images.
  const bool inline_candidate =
      meta.height <= (3 * line_height) && inline_width <= (3 * line_height);

  const bool wide_band_candidate =
      meta.width >= (meta.height * 3) &&
      DivRoundUp(meta.height * text_width, meta.width) <= band_max_height;
  const bool block_band_candidate =
      meta.width <= (6 * line_height) && meta.height <= (6 * line_height);
  const bool leading_paragraph_band_candidate =
      leading_paragraph_image && meta.width <= (8 * line_height) &&
      meta.height <= (8 * line_height);
  const bool medium_band_candidate =
      DivRoundUp(meta.height * text_width, meta.width) <= band_max_height;
  const bool caption_friendly_start =
      leading_paragraph_image || (req.current_screen == 0 && IsAtScreenStart(req));
  int caption_baseline = req.pen_y;
  if (req.line_began)
    caption_baseline += line_height + req.linespacing;
  const int caption_band_max_height =
      limit_y - caption_baseline - req.linespacing -
      (figure_with_caption ? min_caption_text_height : min_follow_text_height);
  int next_page_break_probe = 0;
  const int next_screen_height =
      NextScreenHeight(req, &next_page_break_probe);
  const int next_limit_y = next_screen_height - next_margin_bottom;
  const int fresh_baseline = req.margin_top + line_height;
  const int next_caption_band_max_height =
      next_limit_y - fresh_baseline - req.linespacing -
      min_caption_text_height;
  const int current_follow_band_max_height =
      limit_y - caption_baseline - req.linespacing - min_follow_text_height;
  const int next_follow_band_max_height =
      next_limit_y - fresh_baseline - req.linespacing - min_follow_text_height;
  const int full_width_band_height =
      std::max(1, DivRoundUp(meta.height * text_width, meta.width));
  const bool current_figure_band_candidate =
      figure_with_caption && caption_band_max_height >= (5 * line_height);
  const bool figure_band_candidate =
      current_figure_band_candidate ||
      (figure_with_caption && next_caption_band_max_height >=
                                  (5 * line_height));
  const bool leading_caption_band_candidate =
      !figure_with_caption && caption_friendly_start &&
      caption_band_max_height >= (5 * line_height);
  // MOBI photo inserts often arrive as plain <img> blocks without enough
  // structural context to be classified as figures. If the image scaled to the
  // text width still leaves room for follow-up prose on this or the next
  // screen, treat it as a band instead of forcing page mode.
  const bool flow_text_band_candidate =
      full_width_band_height <=
      std::max(current_follow_band_max_height, next_follow_band_max_height);
  const bool band_candidate =
      wide_band_candidate || block_band_candidate ||
      leading_paragraph_band_candidate || medium_band_candidate ||
      leading_caption_band_candidate || figure_band_candidate ||
      flow_text_band_candidate;

  if (inline_candidate) {
    plan.mode = INLINE_IMAGE_LAYOUT_INLINE;
    plan.draw_width = inline_width;
    plan.draw_height = inline_height;
    plan.consume_rest_of_screen = false;
    plan.vertical_space_after_draw = 0;
    plan.advance_before = false;
    plan.page_breaks = 0;
    plan.next_text_screen = req.current_screen;

    const int remaining_width =
        text_width - std::max(0, req.pen_x - req.margin_left);
    if (remaining_width < inline_width) {
      if (text_width >= inline_width) {
        plan.line_break_before = true;
      } else if (band_candidate) {
        plan.mode = INLINE_IMAGE_LAYOUT_BAND;
      } else {
        FillPageMode(req, &plan);
        return plan;
      }
    } else {
      plan.line_break_before = false;
    }

    if (plan.mode == INLINE_IMAGE_LAYOUT_INLINE)
      return plan;
  }

  if (band_candidate) {
    int band_width = 0;
    int band_height = 0;
    if (wide_band_candidate) {
      band_width = text_width;
      band_height = std::max(1, DivRoundUp(meta.height * text_width, meta.width));
    } else if (block_band_candidate || leading_paragraph_band_candidate) {
      FitWithinBoxNoUpscale(meta.width, meta.height, text_width,
                            block_band_max_height, &band_width, &band_height);
    } else if (figure_band_candidate || leading_caption_band_candidate) {
      const int figure_fit_max_height =
          current_figure_band_candidate ? caption_band_max_height
                                        : next_caption_band_max_height;
      FitWithinBoxNoUpscale(meta.width, meta.height, text_width,
                            figure_fit_max_height, &band_width, &band_height);
    } else if (flow_text_band_candidate) {
      FitWithinBoxNoUpscale(meta.width, meta.height, text_width,
                            std::max(current_follow_band_max_height,
                                     next_follow_band_max_height),
                            &band_width, &band_height);
    } else {
      FitWithinBoxNoUpscale(meta.width, meta.height, text_width,
                            band_max_height, &band_width, &band_height);
    }

    plan.mode = INLINE_IMAGE_LAYOUT_BAND;
    plan.draw_width = band_width;
    plan.draw_height = band_height;
    plan.line_break_before = req.line_began;
    plan.consume_rest_of_screen = false;
    plan.vertical_space_after_draw = band_height + req.linespacing;
    plan.advance_before = false;
    plan.page_breaks = 0;
    plan.next_text_screen = req.current_screen;

    int baseline = req.pen_y;
    if (plan.line_break_before)
      baseline += line_height + req.linespacing;
    if (baseline + band_height + req.linespacing > limit_y) {
      int page_breaks = 0;
      int next_height = NextScreenHeight(req, &page_breaks);
      int next_limit_y = next_height - req.margin_bottom;
      int fresh_baseline = req.margin_top + line_height;
      if (fresh_baseline + band_height + req.linespacing <= next_limit_y) {
        plan.advance_before = true;
        plan.page_breaks = page_breaks;
        plan.next_text_screen = (req.current_screen == 1) ? 0 : 1;
        plan.line_break_before = false;
        return plan;
      }

      FillPageMode(req, &plan);
      return plan;
    }

    if (req.current_screen == 1 && figure_with_caption) {
      const int remaining_after_band = limit_y - (baseline + band_height);
      if (remaining_after_band < min_caption_text_height) {
        int page_breaks = 0;
        int next_height = NextScreenHeight(req, &page_breaks);
        int next_limit_y = next_height - req.margin_bottom;
        int fresh_baseline = req.margin_top + line_height;
        if (fresh_baseline + band_height + req.linespacing +
                min_caption_text_height <=
            next_limit_y) {
          plan.advance_before = true;
          plan.page_breaks = page_breaks;
          plan.next_text_screen = 0;
          plan.line_break_before = false;
          return plan;
        }
      }
    }

    // Avoid leaving medium block-like images orphaned with their paragraph text
    // pushed to the next screen when the block itself could move forward.
    if (req.current_screen == 0 &&
        (leading_paragraph_image || figure_with_caption ||
         flow_text_band_candidate) &&
        !wide_band_candidate && !IsAtScreenStart(req)) {
      int remaining_after_band = limit_y - (baseline + band_height);
      const int min_remaining_after_band =
          figure_with_caption ? min_caption_text_height : min_follow_text_height;
      if (remaining_after_band < min_remaining_after_band) {
        int page_breaks = 0;
        int next_height = NextScreenHeight(req, &page_breaks);
        int next_limit_y = next_height - req.margin_bottom;
        int fresh_baseline = req.margin_top + line_height;
        if (fresh_baseline + band_height + req.linespacing +
                min_remaining_after_band <=
            next_limit_y) {
          plan.advance_before = true;
          plan.page_breaks = page_breaks;
          plan.next_text_screen = (req.current_screen == 1) ? 0 : 1;
          plan.line_break_before = false;
          return plan;
        }
      }
    }

    return plan;
  }

  FillPageMode(req, &plan);
  return plan;
}
