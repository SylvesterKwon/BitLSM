#pragma once
// Minimal compatibility shim for folly/Portability.h.
// Authored for BitLSM; NOT Folly code. Provides only the macros/constants that
// the vendored folly TDigest sources reference. See ../../NOTICE.
#include <cstddef>

#if defined(__GNUC__) || defined(__clang__)
#define FOLLY_ALWAYS_INLINE inline __attribute__((__always_inline__))
#define FOLLY_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define FOLLY_LIKELY(x) (__builtin_expect(!!(x), 1))
#else
#define FOLLY_ALWAYS_INLINE inline
#define FOLLY_UNLIKELY(x) (x)
#define FOLLY_LIKELY(x) (x)
#endif

namespace folly {
#ifdef NDEBUG
constexpr bool kIsDebug = false;
#else
constexpr bool kIsDebug = true;
#endif
}  // namespace folly
