#pragma once
#include <string>
class Book;
namespace library_cover {
enum class Status {Cached,Prepared,Absent,Unsupported,Error,WriteError,Cancelled,NeedsMetadata,LowMemory,NeedsPreparation};
struct Result {Status status;int code;bool heavy;};
// Main-thread source/cache operation; no visibility/idle policy. Keeps browser
// attempt semantics and leaves a usable thumbnail on a failed cache save.
// cache_only admits no decoding or writes; a source miss returns NeedsPreparation.
Result Prepare(Book *,const std::string &,bool is_new_3ds,bool selected,bool cache_only=false);
}
