#include "book/book.h"
#include "ui/text.h"

#include <type_traits>

// These owners release resources in their destructors; copying must never
// duplicate the owning pointers. BookContext only borrows services.
static_assert(!std::is_copy_constructible<Book>::value,
              "Book must not copy ownership of pages, images and backend state");
static_assert(!std::is_copy_assignable<Book>::value,
              "Book assignment must not overwrite or alias owned resources");
static_assert(!std::is_copy_constructible<Text>::value,
              "Text must not copy ownership of framebuffers and render services");
static_assert(!std::is_copy_assignable<Text>::value,
              "Text assignment must not overwrite or alias owned resources");
static_assert(std::is_copy_constructible<BookContext>::value,
              "BookContext must remain a copyable non-owning context");
static_assert(std::is_copy_assignable<BookContext>::value,
              "BookContext must remain assignable without owning services");

int main() { return 0; }
