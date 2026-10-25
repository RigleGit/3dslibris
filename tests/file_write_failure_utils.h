#pragma once

#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace file_write_failure {

// A small buffered write succeeds; its final flush fails against a real file.
// Keep the process limit isolated from the rest of the test runner.
template <typename Write> bool Run(Write write) {
  const pid_t child = fork();
  if (child < 0)
    return false;
  if (child == 0) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_FSIZE, &limit) != 0)
      _exit(2);
    limit.rlim_cur = 0;
    std::signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
      _exit(2);
    write();
    _exit(0);
  }
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace file_write_failure
