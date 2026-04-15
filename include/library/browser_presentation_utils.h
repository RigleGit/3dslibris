#pragma once

#include <string>

class Book;
class Text;

namespace browser_presentation_utils {

std::string BuildBrowserDisplayName(Book *book);
size_t Utf8BytesForCharCount(const char *s, size_t char_count);
void DrawWrappedTitleInsideCover(Text *ts, const std::string &title,
                                 int x, int y, int w, int h,
                                 unsigned char style);

} // namespace browser_presentation_utils
