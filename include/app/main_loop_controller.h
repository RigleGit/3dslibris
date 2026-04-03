#pragma once

class App;

class MainLoopController {
public:
  explicit MainLoopController(App &app);
  int RunMainLoop();

private:
  App &app_;
};
