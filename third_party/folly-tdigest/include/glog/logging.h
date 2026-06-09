#pragma once
// Minimal glog shim — maps only the CHECK/DCHECK macros the vendored folly
// TDigest sources use onto <cassert>. None of those uses stream with "<<".
// Authored for BitLSM; NOT glog code. See ../../NOTICE.
#include <cassert>

#ifndef CHECK
#define CHECK(cond) assert(cond)
#endif
#define DCHECK(cond) assert(cond)
#define DCHECK_EQ(a, b) assert((a) == (b))
#define DCHECK_NE(a, b) assert((a) != (b))
#define DCHECK_LT(a, b) assert((a) < (b))
#define DCHECK_LE(a, b) assert((a) <= (b))
#define DCHECK_GT(a, b) assert((a) > (b))
#define DCHECK_GE(a, b) assert((a) >= (b))
