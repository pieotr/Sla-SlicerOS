#!/usr/bin/env bash
# Runs INSIDE the tools/docker/Dockerfile.build container (invoked by build_portable.sh).
# /src-ro = read-only repo, /work = persistent work dir (build trees, copy of the sources), /out = output.
set -euo pipefail
JOBS="${JOBS:-8}"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS" MAKEFLAGS="-j$JOBS"
if [[ -s /work/repo-files.lst ]]; then
  # Exactly the repository file set computed by build_portable.sh on the host (NUL-separated list).
  # Everything in the copy except the persistent build tree is replaced.
  mkdir -p /work/src
  find /work/src -mindepth 1 -maxdepth 1 ! -name build -exec rm -rf {} +
  rsync -a --from0 --files-from=/work/repo-files.lst /src-ro/ /work/src/
else
  # Source tarball without git: copy everything except build output. Build trees are excluded, anchored to the
  # repo root ("/..."); build-utils/ (it defines the encoding_check() CMake function) matches '/build-*/' too,
  # so it is included first (rsync: first match wins).
  rsync -a --delete --include /build-utils/ --exclude /build --exclude '/build-*/' --exclude /deps/build \
        --exclude '/deps/build-*/' --exclude /dist --exclude /.git /src-ro/ /work/src/
fi
cd /work/src
DEPS=/work/deps-build; APP=/work/src/build;   # project-local: some tests require a build tree inside the sources
 PREFIX="$DEPS/destdir/usr/local"

if [[ ! -f "$PREFIX/lib/libwx_gtk3u_core-3.2.a" ]]; then
  echo "== deps superbuild =="
  cmake -S deps -B "$DEPS" -DDEP_WX_GTK3=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release
  cmake --build "$DEPS" -- -j1          # inner builds are parallelised by the dep scripts
fi
echo "== app =="
cmake -S . -B "$APP" -DCMAKE_BUILD_TYPE=Release -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DSLIC3R_PCH=OFF \
      -DSLIC3R_BUILD_TESTS=ON "-DSLA_SLICER_BUILD_ID=${SLA_SLICER_BUILD_ID:-}" \
      '-DSLIC3R_TEST_SUITES=arrange;thumbnails;libslic3r;sla_print' -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DOpenCV_DIR=/opt/opencv/lib/cmake/opencv4
cmake --build "$APP" -- -j"$JOBS"
echo "== ctest (no display) =="
(cd "$APP" && ctest --output-on-failure -j4 -LE "oracle|gui-runtime|gui") 2>&1 | tee /work/ctest.log | tail -25 || true
echo "== bundle =="
rm -rf /work/src/dist
BUNDLE_LIBSTDCXX="${BUNDLE_LIBSTDCXX:-0}" bash tools/make_relocatable_bundle.sh "$APP"
rm -rf /out/sla-slicer-* ; cp -a /work/src/dist/. /out/
chown -R "${HOST_UID:-0}:${HOST_GID:-0}" /out /work 2>/dev/null || true
ls -la /out
