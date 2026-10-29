#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace library_search {
enum class Field { Name, Title, Author };
enum class Status { Idle, Searching, Complete, Limited, Cancelled };
enum class MetadataStatus { Ready, Unsupported, Error, Cancelled };
struct Entry { std::string path, name; bool directory; };
struct Result : Entry { std::string label, author; };
struct MetadataValue {
  std::string title, author;
  bool extracted=false, cache_saved=true;
};
typedef bool (*RootSource)(void *,size_t,Entry *);
typedef MetadataStatus (*MetadataSource)(void *,const std::string&,MetadataValue *);
typedef uint64_t (*Clock)();

// Roots are borrowed through the source until Cancel/completion. Descendants
// are direct files only; neither results nor the cursor own any Book objects.
class Engine {
public:
  Engine(); ~Engine();
  bool Start(Field,const std::string&,RootSource,void *,bool child_files,
             MetadataSource=nullptr);
  void Pump(unsigned entries=16,Clock clock=nullptr,unsigned milliseconds=4);
  void Cancel();
  Status GetStatus() const;
  const std::vector<Result> &Results() const;
  size_t Examined() const;
  size_t Errors() const;
  size_t Unsupported() const;
  size_t Uncached() const;
  size_t AuxiliaryBytes() const;
private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace library_search
