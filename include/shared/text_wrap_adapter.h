#pragma once

#include <string>
#include <vector>

#include "shared/text_unicode_utils.h"
#include "shared/text_wrap_utils.h"

// Binds text_wrap_utils::WrapUtf8 to a typesetter's glyph metrics. Templated on
// the typesetter so host tests can supply fixed-width stubs.
namespace text_wrap_adapter {

template <class T> struct FitContext {
  T *ts;
  int style;
  int width;
};

template <class T>
size_t FitBytes(const char *s, size_t, void *ctx) {
  FitContext<T> *f = static_cast<FitContext<T> *>(ctx);
  int w = f->width < 1 ? 1 : (f->width > 255 ? 255 : f->width);
  const int chars = f->ts->GetCharCountInsideWidth(s, f->style, w);
  return text_unicode_utils::Utf8BytesForDisplayChars(s, (size_t)chars);
}

// Wraps `s` to `width` pixels in `style`. Lines are byte ranges into `s`.
template <class T>
void Wrap(T *ts, int style, int width, const std::string &s,
          std::vector<text_wrap_utils::Line> *out) {
  FitContext<T> ctx = {ts, style, width};
  text_wrap_utils::WrapUtf8(s.c_str(), s.size(), &FitBytes<T>, &ctx, out);
}

} // namespace text_wrap_adapter
