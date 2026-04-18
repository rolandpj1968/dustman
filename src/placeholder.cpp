// Placeholder source for the dustman library target. Ensures the archive
// has at least one object file so picky linkers (notably MSVC's) do not
// complain. Removed when the first real implementation file lands.

#include "dustman/dustman.hpp"

namespace dustman::detail {

int placeholder_symbol() noexcept { return 0; }

}  // namespace dustman::detail
