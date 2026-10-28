#pragma once

#include <3ds.h>

class HomeButtonGuard {
public:
  HomeButtonGuard() : was_allowed_(aptIsHomeAllowed()) {
    aptSetHomeAllowed(false);
  }

  ~HomeButtonGuard() {
    aptSetHomeAllowed(was_allowed_);
  }

private:
  bool was_allowed_;
  HomeButtonGuard(const HomeButtonGuard &);
  HomeButtonGuard &operator=(const HomeButtonGuard &);
};
