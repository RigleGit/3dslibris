#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
namespace library_walk {
struct Entry {std::string path,name;bool directory;};
typedef bool (*RootSource)(void *,size_t,Entry *);
typedef uint64_t (*Clock)();
enum class Step {Item,Skip,End};
// Borrow visible roots; walk their direct files only. Owns one directory cursor.
class Cursor {
public:
  Cursor();~Cursor();
  bool Start(RootSource,void *,bool child_files);
  Step Next(Entry *);
  void Close();
  size_t Errors() const;
private:
  struct State;std::unique_ptr<State> state_;
};
}
