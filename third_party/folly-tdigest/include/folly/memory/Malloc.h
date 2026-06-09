#pragma once
// Minimal compatibility shim for folly/memory/Malloc.h.
// Authored for BitLSM; NOT Folly code. See ../../../NOTICE.
#include <cstddef>
#include <cstdlib>
#include <new>

namespace folly {

inline void* checkedMalloc(size_t size) {
  void* p = std::malloc(size);
  if (!p) {
    throw std::bad_alloc();
  }
  return p;
}

inline void sizedFree(void* p, size_t /* size */) {
  std::free(p);
}

}  // namespace folly
