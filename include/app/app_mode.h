#pragma once

#include <3ds/types.h>

enum class AppMode : u8
{
  Book = 0,
  Browser = 1,
  Prefs = 2,
  PrefsFont = 3,
  PrefsFontBold = 4,
  PrefsFontItalic = 5,
  PrefsFontBoldItalic = 6,
  Quit = 7,
  Bookmarks = 8,
  Chapters = 9,
  Opening = 10,
  BookInfo = 11,
  Search = 12,
  ImageViewer = 13,
};
