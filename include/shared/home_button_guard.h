#pragma once

#include <3ds.h>

class HomeButtonGuard {
public:
  HomeButtonGuard() {
    aptSetHomeAllowed(false);
  }

  ~HomeButtonGuard() {
    aptSetHomeAllowed(true);
  }

private:
  HomeButtonGuard(const HomeButtonGuard &);
  HomeButtonGuard &operator=(const HomeButtonGuard &);
};
