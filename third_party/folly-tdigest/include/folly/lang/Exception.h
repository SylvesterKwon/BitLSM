#pragma once
// Minimal compatibility shim for folly/lang/Exception.h.
// Authored for BitLSM; NOT Folly code. See ../../../NOTICE.
#include <utility>

namespace folly {

// throw_exception<Ex>(args...) — construct and throw Ex from args.
template <class Ex, class... Args>
[[noreturn]] void throw_exception(Args&&... args) {
  throw Ex(std::forward<Args>(args)...);
}

// throw_exception(ex) — throw an already-constructed exception object.
template <class Ex>
[[noreturn]] void throw_exception(Ex&& ex) {
  throw std::forward<Ex>(ex);
}

}  // namespace folly
