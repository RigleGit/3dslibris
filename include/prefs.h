#pragma once

#include "app.h"
#include "expat.h"


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

private:
  App *app;
  void Init();
};
