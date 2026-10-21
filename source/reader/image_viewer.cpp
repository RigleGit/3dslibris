#include "app/reader_controller.h"
#include "reader/image_viewer_session.h"
#include "reader/image_view_utils.h"
#include "app/app.h"
#include "book/book.h"
#include "book/page.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include "formats/common/fixed_layout_screen_constants.h"
#include "reader/book_page_nav.h"
#include "shared/debug_log.h"
#include "shared/string_utils.h"
#include "shared/orientation_utils.h"
#include "shared/touch_map_utils.h"
#include "shared/text_unicode_utils.h"
#include "ui/text.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace {
// Preserve reader text state, since closing the viewer must not change layout.
struct TextState {
  Text *text;
  int size, style, bottom;
  bool wrap;
  explicit TextState(Text *t)
      : text(t), size(t->GetPixelSize()), style(t->GetStyle()),
        bottom(t->margin.bottom), wrap(t->IsAutoWrapEnabled()) {
    t->margin.bottom = 0;
    t->SetStyle(TEXT_STYLE_BROWSER);
    t->SetPixelSize(10);
    t->SetAutoWrapEnabled(false);
  }
  ~TextState() {
    text->SetAutoWrapEnabled(wrap);
    text->SetPixelSize(size);
    text->SetStyle(style);
    text->margin.bottom = bottom;
  }
};
void Blit(Text *ts, u16 *screen, int height, int x, int y, int w, int h,
          const raster_image::Bitmap &bitmap, int cx, int cy, int cw, int ch,
          bool quality) {
  fixed_layout_blit_utils::BlitRgb565BitmapScaledCrop(
      ts, screen, height, x, y, w, h, bitmap.pixels.get(), bitmap.PixelCount(),
      bitmap.width, bitmap.height, cx, cy, cw, ch, quality);
}

// Browser labels use a fixed 12px font with a 14px line height. Leave room
// for two lines, descenders and padding inside the rounded button border,
// regardless of the book's current font size.
const int kControlHeight = 44;
const int kControlGap = 4;
const int kControlFooter = 6 + 2 * kControlHeight + kControlGap;

// Reserve the same footer in the decoder's preview buffer and in the draw path.
// Two rows of buttons fit both the 240px portrait and 320px landscape panel.
pdf_view_utils::PreviewLayout OverviewLayout(const ReaderImageViewerSession &s,
                                             int width, int height) {
  return pdf_view_utils::ComputePreviewLayoutInBounds(
      s.image.original_width, s.image.original_height, 6, 6, width - 12,
      height - kControlFooter - 36);
}

void LayoutControls(ReaderImageViewerSession &s, int width, int height) {
  const int button_width = (width - 20) / 3;
  for (int i = 0; i < ReaderImageViewerSession::ControlCount; ++i) {
    Button &button = s.controls[i];
    button.Move(6 + (i % 3) * (button_width + kControlGap),
                height - kControlFooter + (i / 3) * (kControlHeight + kControlGap));
    button.Resize(button_width, kControlHeight);
    button.SetEnabled(i >= ReaderImageViewerSession::Back ||
                      (s.image.pixels && !s.loading && !s.suspended));
  }
  s.controls[ReaderImageViewerSession::ZoomIn].SetEnabled(
      s.controls[ReaderImageViewerSession::ZoomIn].IsEnabled() &&
      s.viewport.zoom_index < s.viewport.max_zoom_index);
  s.controls[ReaderImageViewerSession::ZoomOut].SetEnabled(
      s.controls[ReaderImageViewerSession::ZoomOut].IsEnabled() &&
      s.viewport.zoom_index > 2);
}

void DrawViewerStatus(Text *ts, const char *message, int width, int height) {
  // Status/error messages get two bounded lines above the controls. Button
  // labels use Button's own UTF-8 clipping, like the other menus.
  for (int line = 0; line < 2 && *message; ++line) {
    const u8 count = ts->GetCharCountInsideWidth(message, TEXT_STYLE_BROWSER,
                                                 (u8)std::min(255, width - 12));
    const size_t bytes =
        text_unicode_utils::Utf8BytesForDisplayChars(message, count);
    if (!bytes)
      break;
    ts->SetPen(6, height - kControlFooter - 18 + line * 12);
    ts->PrintString(std::string(message, bytes).c_str());
    message += bytes;
  }
}
} // namespace

bool ReaderController::ImageViewerValid() const {
  if (!image_viewer_)
    return false;
  const ReaderImageViewerSession &s = *image_viewer_;
  Book *book = app_.GetCurrentBook();
  if (!book || book != s.book || !book->GetPage())
    return false;
  const Page *page = book->GetPage();
  return s.token + 1 < (unsigned)page->length &&
         page->buf[s.token] == TEXT_IMAGE &&
         page->buf[s.token + 1] == s.image_id &&
         book->GetOpenSessionId() == s.session &&
         book->GetLayoutRevision() == s.revision &&
         book->GetPageCount() == s.pages &&
         (unsigned)book->GetPosition() == s.page &&
         !book->IsAsyncReflowOpenPending() && !app_.BookNeedsRelayout(book);
}

void ReaderController::InvalidateImageViewer() {
  if (!image_viewer_)
    return;
  Book *book = app_.GetCurrentBook();
  if (book && book == image_viewer_->book) {
    book->ClearFocusedInlineLink();
    book->ResetReadingPaceEstimate();
  }
  app_.SetInlineLinkFocusActive(false);
  app_.SetInlineLinkHoldArmed(false);
  app_.SetInlineLinkHoldConsumed(true);
  image_viewer_.reset();
}

void ReaderController::SuspendImageViewer() {
  if (!image_viewer_)
    return;
  ReaderImageViewerSession &s = *image_viewer_;
  s.image = raster_image::Bitmap();
  s.preview = raster_image::Bitmap();
  s.preview_ready = false;
  s.suspended = true;
  s.loading = true;
  s.dirty = true;
  s.wait_release = true;
  s.viewport.interaction_active = false;
  s.touch_panning = false;
}

void ReaderController::OpenImageViewer() {
  Book *book = app_.GetCurrentBook();
  if (!book ||
      (!HasExtCI(book->GetFileName(), ".epub") && !book->IsMobiFile() &&
       !HasExtCI(book->GetFileName(), ".fb2")) ||
      !book->GetPageCount() || book->IsAsyncReflowOpenPending() ||
      app_.ShouldAbortWork() || app_.BookNeedsRelayout(book))
    return;
  if (book->GetPosition() >= book->GetPageCount())
    return;
  Page *page = book->GetPage();
  if (!page)
    return;
  const auto &items = page->GetRenderedSelectables();
  const int selected = book->GetFocusedInlineLinkIndex();
  if (selected < 0 || selected >= (int)items.size() ||
      items[selected].kind != Page::IMAGE)
    return;
  const auto target = items[selected];
  InvalidateSearch(false);
  image_viewer_.reset(new (std::nothrow) ReaderImageViewerSession);
  if (!image_viewer_) {
    app_.PrintStatus("Image viewer memory unavailable");
    return;
  }
  ReaderImageViewerSession &s = *image_viewer_;
  s.book = book;
  s.session = book->GetOpenSessionId();
  s.revision = book->GetLayoutRevision();
  s.pages = book->GetPageCount();
  s.page = book->GetPosition();
  s.selection = selected;
  s.token = target.token_offset;
  s.image_id = target.image_id;
  s.viewport.max_zoom_index = 6;
  static const char *labels[] = {"zoom +", "zoom -",  "fit",
                                 "back",   "library", "settings"};
  static const char *keys[] = {"(A)", "(B)",     "(X)",
                               "(Y)", "(START)", "(SELECT)"};
  for (int i = 0; i < ReaderImageViewerSession::ControlCount; ++i) {
    s.controls[i].Init(app_.ts.get());
    s.controls[i].SetStyle(BUTTON_STYLE_SETTING);
    s.controls[i].SetIcon(UI_BUTTON_ICON_NONE);
    s.controls[i].Label(labels[i]);
    s.controls[i].SetLabel2(keys[i]);
  }
  app_.ResetPageRepeat();
  app_.SetMode(AppMode::ImageViewer);
  // Loading starts on the next frame, after the indication is presented.
  DrawImageViewer();
}

bool ReaderController::LoadImageViewer(int zoom) {
  if (!ImageViewerValid() || app_.ShouldAbortWork())
    return false;
  ReaderImageViewerSession &s = *image_viewer_;
  s.loading = true;
  s.message = "Loading image...";
  DrawImageViewer();
  app_.PresentIfDirty();
  if (app_.ShouldAbortWork() || !ImageViewerValid())
    return false;
  const uint64_t started = osGetTime();
  image_memory::Bytes bytes;
  const bool read = s.book->ReadInlineImageSource(s.image_id, &bytes);
  const uint64_t read_ms = osGetTime() - started;
  const auto target = fixed_layout_screen::TargetDims(app_.orientation, true);
  int width = s.image.original_width, height = s.image.original_height;
  InlineImageMetadata metadata{};
  if ((width <= 0 || height <= 0) &&
      s.book->GetInlineImageMetadata(s.image_id, &metadata)) {
    width = metadata.width;
    height = metadata.height;
  }
  // If probing failed, decode at least the requested screen resolution. The
  // decoder still validates its own original dimensions and allocation bounds.
  const auto required =
      width > 0 && height > 0
          ? image_view::RequiredSize(width, height, target.width, target.height,
                                     zoom)
          : image_view::Size{target.width * 4, target.height * 4};
  const char *error = "Image unavailable (resource/memory limit)";
  const bool decoded =
      read && !app_.ShouldAbortWork() &&
      raster_image::Decode(bytes.data(),bytes.size(),required.width, required.height,
                           app_.ts->GetBgColor(), &s.image, &error);
  s.loading = false;
  s.suspended = false;
  if (decoded) {
    s.viewport.zoom_index = zoom;
    s.message.clear();
    if (!s.preview.pixels) {
      const auto bottom =
          fixed_layout_screen::TargetDims(app_.orientation, false);
      const auto preview = OverviewLayout(s, bottom.width, bottom.height);
      if (s.preview.Allocate(std::max(1, preview.width),
                             std::max(1, preview.height))) {
        // The first filtered overview draw populates this compact buffer.
        s.preview_ready = false;
      }
    }
  } else
    s.message = error ? error : "Image load failed";
  s.dirty = true;
  DBG_LOGF(&app_,
           "IMAGE load ok=%d read_ms=%llu decode_ms=%llu bytes=%lu "
           "bitmap_bytes=%lu decoder_peak=%lu zoom=%d",
           decoded ? 1 : 0, (unsigned long long)read_ms,
           (unsigned long long)(osGetTime() - started - read_ms),
           (unsigned long)bytes.size(),
           (unsigned long)(s.image.PixelCount() * sizeof(u16)),
           (unsigned long)s.image.decoder_peak, zoom);
  return decoded;
}

void ReaderController::CloseImageViewer() {
  Book *book = app_.GetCurrentBook();
  const bool valid = ImageViewerValid();
  const int selected = valid ? image_viewer_->selection : -1;
  InvalidateImageViewer();
  if (!book) {
    app_.ShowLibraryView();
    return;
  }
  book->ResetReadingPaceEstimate();
  book->SetFocusedInlineLinkIndex(selected);
  app_.SetInlineLinkFocusActive(selected >= 0);
  app_.SetInlineLinkHoldArmed(false);
  app_.SetInlineLinkHoldConsumed(true);
  app_.ResetPageRepeat();
  app_.ShowCurrentBookView();
  book_nav::DrawPage(book, app_.ts.get());
}

void ReaderController::DrawImageViewer() {
  if (!ImageViewerValid() || app_.ShouldAbortWork())
    return;
  ReaderImageViewerSession &s = *image_viewer_;
  Text *ts = app_.ts.get();
  const uint64_t started = osGetTime();
  app_.ApplyRenderOrientation(app_.orientation);
  TextState saved(ts);
  const auto top = fixed_layout_screen::TargetDims(app_.orientation, true);
  const auto bottom = fixed_layout_screen::TargetDims(app_.orientation, false);
  ts->SetScreen(ts->screenleft);
  ts->ClearScreen();
  ts->SetScreen(ts->screenright);
  ts->ClearScreen();
  LayoutControls(s, bottom.width, bottom.height);
  if (s.image.pixels) {
    auto rect = image_view::Viewport(
        s.image.original_width, s.image.original_height, top.width, top.height,
        s.viewport.zoom_index, s.viewport.center_x, s.viewport.center_y);
    s.viewport.center_x = rect.left + rect.width / 2;
    s.viewport.center_y = rect.top + rect.height / 2;
    const int cx =
        std::min(s.image.width - 1, (int)(rect.left * s.image.width));
    const int cy =
        std::min(s.image.height - 1, (int)(rect.top * s.image.height));
    const int cw =
        std::max(1, std::min(s.image.width - cx,
                             (int)(rect.width * s.image.width + 0.5f)));
    const int ch =
        std::max(1, std::min(s.image.height - cy,
                             (int)(rect.height * s.image.height + 0.5f)));
    // Fit the visible source rectangle without stretching its aspect ratio.
    const auto placement = pdf_view_utils::ComputePreviewLayout(
        s.image.original_width * rect.width,
        s.image.original_height * rect.height, top.width, top.height);
    Blit(ts, ts->screenleft, top.height, placement.x, placement.y,
         placement.width, placement.height, s.image, cx, cy, cw, ch,
         !s.viewport.interaction_active);
    s.overview = OverviewLayout(s, bottom.width, bottom.height);
    if (s.preview.pixels && !s.preview_ready) {
      Blit(ts, ts->screenright, bottom.height, s.overview.x, s.overview.y,
           s.overview.width, s.overview.height, s.image, 0, 0, s.image.width,
           s.image.height, true);
      const int stride = ts->BufferStride();
      for (int y = 0; y < s.preview.height; ++y)
        std::copy(ts->screenright + (y + s.overview.y) * stride + s.overview.x,
                  ts->screenright + (y + s.overview.y) * stride + s.overview.x +
                      s.preview.width,
                  s.preview.pixels.get() + (size_t)y * s.preview.width);
      s.preview_ready = true;
    } else if (s.preview.pixels) {
      Blit(ts, ts->screenright, bottom.height, s.overview.x, s.overview.y,
           s.overview.width, s.overview.height, s.preview, 0, 0,
           s.preview.width, s.preview.height, true);
    } else {
      Blit(ts, ts->screenright, bottom.height, s.overview.x, s.overview.y,
           s.overview.width, s.overview.height, s.image, 0, 0, s.image.width,
           s.image.height, true);
    }
    ts->SetScreen(ts->screenright);
    ts->DrawRect(
        s.overview.x + (int)(rect.left * s.overview.width),
        s.overview.y + (int)(rect.top * s.overview.height),
        s.overview.x + (int)((rect.left + rect.width) * s.overview.width),
        s.overview.y + (int)((rect.top + rect.height) * s.overview.height),
        ts->GetFgColor());
  }
  ts->SetScreen(ts->screenright);
  DrawViewerStatus(ts,
                   s.loading           ? "Loading image..."
                   : s.message.empty() ? "move: D-pad, Circle Pad / stylus"
                                       : s.message.c_str(),
                   bottom.width, bottom.height);
  for (int i = 0; i < ReaderImageViewerSession::ControlCount; ++i)
    s.controls[i].Draw(false);
  ts->MarkAllScreensDirty();
  s.dirty = false;
  DBG_LOGF(&app_,
           "IMAGE draw ms=%llu quality=%d zoom=%d bitmap_bytes=%lu "
           "preview_bytes=%lu",
           (unsigned long long)(osGetTime() - started),
           s.viewport.interaction_active ? 0 : 1, s.viewport.zoom_index,
           (unsigned long)(s.image.PixelCount() * sizeof(u16)),
           (unsigned long)(s.preview.PixelCount() * sizeof(u16)));
}

void ReaderController::RunImageViewerFrame(const FrameInput &input) {
  if (app_.ShouldAbortWork())
    return;
  if (!ImageViewerValid()) {
    CloseImageViewer();
    return;
  }
  ReaderImageViewerSession &s = *image_viewer_;
  if (s.wait_release) {
    if (input.keys_held == 0)
      s.wait_release = false;
    return;
  }
  const u32 keys = input.keys_down, held = input.keys_held;
  const auto touch = input.MapTouch(app_.orientation);
  int control = -1;
  if (keys & KEY_TOUCH) {
    // A gesture belongs either to a button or to the overview from its start.
    // Holding a button, or dragging into the overview from outside, cannot pan.
    s.touch_panning = false;
    for (int i = 0; i < ReaderImageViewerSession::ControlCount; ++i) {
      if (s.controls[i].EnclosesPoint(touch.x, touch.y)) {
        if (s.controls[i].IsEnabled())
          control = i;
        break;
      }
    }
    if (control < 0 && s.image.pixels && touch.x >= s.overview.x &&
        touch.x < s.overview.x + s.overview.width && touch.y >= s.overview.y &&
        touch.y < s.overview.y + s.overview.height)
      s.touch_panning = true;
  } else if (!(held & KEY_TOUCH))
    s.touch_panning = false;
  if ((keys & app_.key.y) || control == ReaderImageViewerSession::Back) {
    CloseImageViewer();
    return;
  }
  if ((keys & app_.key.start) || control == ReaderImageViewerSession::Library) {
    InvalidateImageViewer();
    app_.ShowLibraryView();
    return;
  }
  if ((keys & app_.key.select) ||
      control == ReaderImageViewerSession::Settings) {
    CloseImageViewer();
    app_.ShowSettingsView(true);
    return;
  }
  if (s.loading || s.suspended) {
    LoadImageViewer(s.viewport.zoom_index);
    s.dirty = true;
    if (app_.ShouldAbortWork())
      return;
  }
  if (!s.image.pixels) {
    if (s.dirty)
      DrawImageViewer();
    return;
  }
  int zoom = s.viewport.zoom_index;
  if ((keys & app_.key.a) || control == ReaderImageViewerSession::ZoomIn)
    zoom = std::min(s.viewport.max_zoom_index, zoom + 1);
  if ((keys & app_.key.b) || control == ReaderImageViewerSession::ZoomOut)
    zoom = std::max(2, zoom - 1);
  if ((keys & app_.key.x) || control == ReaderImageViewerSession::Fit) {
    zoom = 2;
    s.viewport.center_x = s.viewport.center_y = 0.5f;
    s.dirty = true;
  }
  if (zoom != s.viewport.zoom_index) {
    const auto top = fixed_layout_screen::TargetDims(app_.orientation, true);
    const auto required = image_view::RequiredSize(s.image.original_width,
                                                   s.image.original_height,
                                                   top.width, top.height, zoom);
    if (s.image.width >= required.width && s.image.height >= required.height) {
      s.viewport.zoom_index = zoom;
      s.message.clear();
    } else
      LoadImageViewer(zoom);
    s.dirty = true;
    if (app_.ShouldAbortWork())
      return;
  }
  circlePosition circle{};
  hidCircleRead(&circle);
  float x = std::abs(circle.dx) > 15 ? circle.dx * 0.0003f : 0;
  float y = std::abs(circle.dy) > 15 ? -circle.dy * 0.0003f : 0;
  if (held & KEY_DLEFT)
    x -= 0.025f;
  if (held & KEY_DRIGHT)
    x += 0.025f;
  if (held & KEY_DUP)
    y -= 0.025f;
  if (held & KEY_DDOWN)
    y += 0.025f;
  bool moving = x != 0 || y != 0;
  if ((held & KEY_TOUCH) && s.touch_panning) {
    const auto top = fixed_layout_screen::TargetDims(app_.orientation, true);
    const auto rect = image_view::Viewport(
        s.image.original_width, s.image.original_height, top.width, top.height,
        s.viewport.zoom_index, s.viewport.center_x, s.viewport.center_y);
    const auto center = pdf_view_utils::RecenterViewportFromPreview(
        s.overview, rect, touch.x, touch.y);
    s.viewport.center_x = center.x;
    s.viewport.center_y = center.y;
    s.dirty = true;
    moving = true;
  } else if (moving) {
    float dx, dy;
    image_view::MapPan(app_.orientation, x, y, &dx, &dy);
    if (fixed_layout_viewport_utils::PanViewport(&s.viewport, dx, dy))
      s.dirty = true;
  }
  if (moving != s.viewport.interaction_active)
    s.dirty = true;
  s.viewport.interaction_active = moving;
  if (s.dirty)
    DrawImageViewer();
}
