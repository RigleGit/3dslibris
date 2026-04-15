/*
    3dslibris - prefs.h
    Adapted from dslibris for Nintendo 3DS.

    Original attribution (dslibris): Ray Haleblian, GPLv2+.
    Modified for Nintendo 3DS by Rigle.

    Summary:
    - Preferences persistence API (read/apply/write).
    - Stores input/display options and per-book state in XML prefs file.
*/

#pragma once

#include "library/browser_view_mode.h"

class App;

class Prefs {
public:
  Prefs(App *app);
  ~Prefs();
  void Apply();
  int Read();
  int Write();
  long modtime;
  bool swapshoulder;
  bool time24h;
  BrowserViewMode browser_view_mode;

private:
  App *app;
  void Init();
};
