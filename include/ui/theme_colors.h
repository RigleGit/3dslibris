#pragma once

#include <3ds.h>
#include "color_utils.h"

struct ThemePalette {
  // Bottom-screen gradient
  float bgTopR, bgTopG, bgTopB;
  float bgBotR, bgBotG, bgBotB;

  // Button fill
  float btnFillTopR, btnFillTopG, btnFillTopB;
  float btnFillBotR, btnFillBotG, btnFillBotB;

  // Button borders
  float btnBorderOuterR, btnBorderOuterG, btnBorderOuterB;
  float btnBorderInnerR, btnBorderInnerG, btnBorderInnerB;

  // Button shadow
  float btnShadowR, btnShadowG, btnShadowB;

  // Button highlight (top sheen)
  float btnHighlightR, btnHighlightG, btnHighlightB;

  // Icon / ink color
  float iconR, iconG, iconB;

  // Text foreground (for menus where TextRenderer isn't used directly)
  u16 textFgColor;
};

inline const ThemePalette &GetThemePalette(int colorMode) {
  static const ThemePalette kLight = {
      // Background gradient: white -> light gray
      255.0f, 255.0f, 255.0f,
      245.0f, 245.0f, 245.0f,

      // Button fill: light gray -> slightly darker gray
      240.0f, 240.0f, 240.0f,
      220.0f, 220.0f, 220.0f,

      // Button borders: dark gray
      80.0f, 80.0f, 80.0f,
      150.0f, 150.0f, 150.0f,

      // Button shadow: darker gray
      60.0f, 60.0f, 60.0f,

      // Highlight: white
      254.0f, 251.0f, 244.0f,

      // Icon: dark gray/black
      30.0f, 30.0f, 30.0f,

      // Text: black
      0x0000};

  static const ThemePalette kDark = {
      // Background gradient: dark gray -> black
      40.0f, 40.0f, 40.0f,
      10.0f, 10.0f, 10.0f,

      // Button fill: dark gray -> darker gray
      55.0f, 55.0f, 55.0f,
      35.0f, 35.0f, 35.0f,

      // Button borders: light gray
      180.0f, 180.0f, 180.0f,
      120.0f, 120.0f, 120.0f,

      // Button shadow: black
      0.0f, 0.0f, 0.0f,

      // Highlight: light gray
      200.0f, 200.0f, 200.0f,

      // Icon: white
      220.0f, 220.0f, 220.0f,

      // Text: white
      0xFFFF};

  static const ThemePalette kSepia = {
      // Background gradient: sepia tones
      244.0f, 226.0f, 195.0f,
      238.0f, 220.0f, 185.0f,

      // Button fill: sepia
      241.0f, 231.0f, 213.0f,
      229.0f, 216.0f, 196.0f,

      // Button borders: brown
      114.0f, 80.0f, 48.0f,
      186.0f, 158.0f, 124.0f,

      // Button shadow: dark brown
      78.0f, 56.0f, 36.0f,

      // Highlight: cream
      254.0f, 251.0f, 244.0f,

      // Icon: brown
      110.0f, 73.0f, 42.0f,

      // Text: dark brown
      RGB565FromU8(70.0f, 52.0f, 32.0f)};

  static const ThemePalette kTrueLight = {
      // Background: pure white (no visible gradient)
      255.0f, 255.0f, 255.0f,
      255.0f, 255.0f, 255.0f,

      // Button fill: white -> very light gray
      250.0f, 250.0f, 250.0f,
      240.0f, 240.0f, 240.0f,

      // Button borders: gray
      100.0f, 100.0f, 100.0f,
      160.0f, 160.0f, 160.0f,

      // Button shadow: medium gray
      80.0f, 80.0f, 80.0f,

      // Highlight: white
      255.0f, 255.0f, 255.0f,

      // Icon: near black
      20.0f, 20.0f, 20.0f,

      // Text: black
      0x0000};

  static const ThemePalette kTrueDark = {
      // Background: pure black
      0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f,

      // Button fill: dark gray -> black
      40.0f, 40.0f, 40.0f,
      20.0f, 20.0f, 20.0f,

      // Button borders: medium gray
      140.0f, 140.0f, 140.0f,
      90.0f, 90.0f, 90.0f,

      // Button shadow: black
      0.0f, 0.0f, 0.0f,

      // Highlight: dark gray
      60.0f, 60.0f, 60.0f,

      // Icon: light gray
      180.0f, 180.0f, 180.0f,

      // Text: light gray
      RGB565FromU8(180.0f, 180.0f, 180.0f)};

  static const ThemePalette kDarkSepia = {
      // Background gradient: dark brown -> darker brown
      80.0f, 60.0f, 40.0f,
      50.0f, 35.0f, 20.0f,

      // Button fill: brown -> dark brown
      70.0f, 50.0f, 32.0f,
      55.0f, 38.0f, 22.0f,

      // Button borders: light brown
      160.0f, 130.0f, 100.0f,
      110.0f, 80.0f, 55.0f,

      // Button shadow: very dark brown
      30.0f, 20.0f, 12.0f,

      // Highlight: cream-brown
      180.0f, 160.0f, 130.0f,

      // Icon: light brown
      190.0f, 160.0f, 120.0f,

      // Text: light brown
      RGB565FromU8(180.0f, 150.0f, 110.0f)};

  switch (colorMode) {
  case 1:
    return kDark;
  case 2:
    return kSepia;
  case 3:
    return kTrueLight;
  case 4:
    return kTrueDark;
  case 5:
    return kDarkSepia;
  default:
    return kLight;
  }
}
