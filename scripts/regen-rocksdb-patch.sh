#!/usr/bin/env bash
# Regenerate patches/rocksdb-bitlsm.patch from the current RocksDB submodule tree.
#
# Workflow for editing RocksDB internals:
#   1. Edit files under third_party/rocksdb/ directly (the patch is already applied
#      to the working tree after a CMake configure).
#   2. Run this script to capture the working-tree changes back into the patch file.
#   3. Commit the updated patches/rocksdb-bitlsm.patch.
#
# To discard local RocksDB edits and return to the pinned vanilla tree:
#   git -C third_party/rocksdb checkout .
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rocksdb_src="${repo_root}/third_party/rocksdb"
patch_file="${repo_root}/patches/rocksdb-bitlsm.patch"

if [ ! -e "${rocksdb_src}/.git" ]; then
  echo "error: ${rocksdb_src} is not a git submodule checkout" >&2
  exit 1
fi

git -C "${rocksdb_src}" diff --full-index > "${patch_file}"

lines=$(wc -l < "${patch_file}")
echo "Regenerated ${patch_file} (${lines} lines)"
git -C "${rocksdb_src}" diff --stat | tail -1
