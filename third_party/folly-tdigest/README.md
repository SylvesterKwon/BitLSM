# folly-tdigest

A self-contained copy of [Folly](https://github.com/facebook/folly)'s
`folly::TDigest` quantile estimator, used by BitLSM to compute continuous-
attribute bin boundaries. It exists so BitLSM does **not** have to build or link
the whole Folly library (which pulls in Boost, fmt, glog, gflags, libevent,
OpenSSL, and more).

## Layout

- `src/`, `include/folly/stats/`, `include/folly/algorithm/BinaryHeap.h` —
  copied **verbatim** from upstream Folly (Apache-2.0 headers intact).
- The remaining headers under `include/folly/` and `include/glog/logging.h` are
  **BitLSM-authored compatibility shims**: minimal re-implementations of just the
  folly/glog interfaces the vendored sources reference. See `NOTICE`.

Because the `folly::` namespace and the `folly/...` include paths are preserved,
consumer code (`src/include/sabi_builder.cpp`) uses `folly::TDigest` /
`folly::Range` unchanged.

## Re-syncing with upstream

1. Edit the `PIN` in `sync-from-upstream.sh` to the desired folly commit.
2. Run `bash sync-from-upstream.sh` (re-fetches only the verbatim files + LICENSE).
3. Rebuild. If the new sources reference folly/glog symbols the shims don't
   provide, the build fails at compile time — extend the relevant shim header.

## License

Folly is Apache-2.0; see `LICENSE` and `NOTICE`.
