#pragma once

#include <string>

class Book;
class Text;

namespace browser_presentation_utils {

std::string BuildBrowserDisplayName(Book *book);
size_t Utf8BytesForCharCount(const char *s, size_t char_count);
// Lines `title` needs inside a box `w` wide, with the same side padding as
// DrawWrappedTitleInsideCover. Callers compare it with the lines they can show.
int CountWrappedTitleLines(Text *ts, const std::string &title, int w,
                           unsigned char style);
void DrawWrappedTitleInsideCover(Text *ts, const std::string &title,
                                 int x, int y, int w, int h,
                                 unsigned char style, int padding_y = 10);

} // namespace browser_presentation_utils
