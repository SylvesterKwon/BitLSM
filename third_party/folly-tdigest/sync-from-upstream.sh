#!/usr/bin/env bash
# Re-fetch the folly files this directory vendors verbatim, pinned to a commit.
# The shim headers (include/folly/Range.h, Utility.h, Overload.h, Portability.h,
# lang/*, memory/Malloc.h, include/glog/logging.h) are BitLSM-authored and are
# NOT touched by this script. See README.md and NOTICE.
set -euo pipefail
command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 1; }
PIN="ef07b7666e6a580a936987fc0f8727c1c51cc374"
BASE="https://raw.githubusercontent.com/facebook/folly/${PIN}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

fetch() {
  mkdir -p "${HERE}/$(dirname "$2")"
  curl -fsSL "${BASE}/$1" -o "${HERE}/$2"
  echo "fetched $1 -> $2"
}

fetch folly/stats/TDigest.h                  include/folly/stats/TDigest.h
fetch folly/stats/detail/DoubleRadixSort.h   include/folly/stats/detail/DoubleRadixSort.h
fetch folly/algorithm/BinaryHeap.h           include/folly/algorithm/BinaryHeap.h
fetch folly/stats/TDigest.cpp                src/TDigest.cpp
fetch folly/stats/detail/DoubleRadixSort.cpp src/DoubleRadixSort.cpp
fetch LICENSE                                LICENSE

echo "Done. Review diffs and rebuild. When bumping folly, update PIN above and"
echo "re-run; if the shims no longer satisfy the new sources, fix them."
