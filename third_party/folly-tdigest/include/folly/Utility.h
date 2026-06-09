#pragma once
// Minimal compatibility shim for folly/Utility.h.
// Authored for BitLSM; NOT Folly code. See ../../NOTICE.
#include <folly/Portability.h>

namespace folly {

// Tag indicating a range of values is already sorted.
struct sorted_equivalent_t {
  explicit sorted_equivalent_t() = default;
};
inline constexpr sorted_equivalent_t sorted_equivalent{};

// Base that deletes copy and move.
struct NonCopyableNonMovable {
  constexpr NonCopyableNonMovable() = default;
  ~NonCopyableNonMovable() = default;
  NonCopyableNonMovable(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable& operator=(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable& operator=(NonCopyableNonMovable&&) = delete;
};

}  // namespace folly
