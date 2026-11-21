#!/usr/bin/env bash
# Reproducible portable Linux build (goal G14). Needs docker + network (downloads deps, apt, PyPI).
# Usage: tools/docker/build_portable.sh [WORK_DIR]      (default WORK_DIR: <repo>/build-docker, gitignored)
# Env:   SLA_SLICER_BUILD_ID=... (default: git describe --always --dirty on the host), JOBS=8 (app parallelism), BUNDLE_LIBSTDCXX=0|1 (bundle libstdc++/libgcc_s), IMAGE=sla-slicer-build:ubuntu22.04
# Result: <WORK_DIR>/dist/sla-slicer-<ver>/ and sla-slicer-<ver>-linux-x86_64.tar.gz. The repo is mounted read-only;
# build trees live in <WORK_DIR>/work, ccache in <WORK_DIR>/ccache (both reused on re-runs).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mkdir -p "${1:-$ROOT/build-docker}" && cd "${1:-$ROOT/build-docker}" && pwd)"
IMAGE="${IMAGE:-sla-slicer-build:ubuntu22.04}"
# The container copy of the tree has no .git, so the build id is computed here and passed in
# (override with SLA_SLICER_BUILD_ID=...; falls back to "source" without git).
BUILD_ID="${SLA_SLICER_BUILD_ID:-$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo source)}"
mkdir -p "$WORK"/{work,ccache,dist}
# The container builds exactly the repository file set (tracked + not-ignored files, as `git ls-files -co
# --exclude-standard` reports them, minus tracked files already deleted on disk), so local QA files, build
# trees and scratch files never leak into the build. Without .git (source tarball) the list is not written and
# container_build.sh falls back to copying the whole tree minus build output.
LIST="$WORK/work/repo-files.lst"
rm -f "$LIST"
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  # tracked files that .gitignore excludes (local QA, download caches) are left out as well
  declare -A IGNORED_TRACKED=()
  while IFS= read -r -d '' f; do IGNORED_TRACKED["$f"]=1; done < <(git -C "$ROOT" ls-files -z -ci --exclude-standard)
  git -C "$ROOT" ls-files -z -co --exclude-standard | while IFS= read -r -d '' f; do
    [[ -n "${IGNORED_TRACKED[$f]:-}" ]] && continue
    [[ -e "$ROOT/$f" || -L "$ROOT/$f" ]] && printf '%s\0' "$f"
  done > "$LIST.tmp"
  mv "$LIST.tmp" "$LIST"
  echo "Repository file set: $(tr -cd '\0' < "$LIST" | wc -c) files -> $LIST"
else
  echo "No git work tree: copying the whole source tree (minus build output)"
fi
docker build -t "$IMAGE" -f "$ROOT/tools/docker/Dockerfile.build" "$ROOT/tools/docker"
docker run --rm -v "$ROOT":/src-ro:ro -v "$WORK/work":/work -v "$WORK/ccache":/ccache -v "$WORK/dist":/out \
  -e SLA_SLICER_BUILD_ID="$BUILD_ID" -e JOBS="${JOBS:-8}" -e BUNDLE_LIBSTDCXX="${BUNDLE_LIBSTDCXX:-0}" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" bash /src-ro/tools/docker/container_build.sh
