#pragma once

#include <string>
#include <vector>

class App;

class StartupController {
public:
  explicit StartupController(App &app);

  // Return codes:
  // 0 -> startup ok, continue main loop
  // 1 -> initialization failed, return error code 1
  // 2 -> fatal boot status shown/handled, return 0
  int RunBootSequence();

private:
  void DrawBootStatus(const char *title, const std::vector<std::string> &lines,
                      bool fatal);
  int HaltOnFatalBootStatus();

  App &app_;
  bool splash_drawn_ = false;
  int splash_orientation_ = -1, splash_color_mode_ = -1;
};
