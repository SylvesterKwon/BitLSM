#pragma once
// Minimal compatibility shim for folly/lang/Builtin.h.
// Authored for BitLSM; NOT Folly code. See ../../../NOTICE.
#ifndef FOLLY_HAS_BUILTIN
#ifdef __has_builtin
#define FOLLY_HAS_BUILTIN(x) __has_builtin(x)
#else
#define FOLLY_HAS_BUILTIN(x) 0
#endif
#endif

#if FOLLY_HAS_BUILTIN(__builtin_unpredictable)
#define FOLLY_BUILTIN_UNPREDICTABLE(exp) __builtin_unpredictable((exp))
#else
#define FOLLY_BUILTIN_UNPREDICTABLE(exp) (exp)
#endif
