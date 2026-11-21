#!/usr/bin/env bash
# Build a relocatable SLA-Slicer tarball from an already built tree (UNTESTED: not run by the author).
#
# Layout produced under dist/sla-slicer-<version>/:
#   sla-slicer            launcher (sets LD_LIBRARY_PATH=lib, execs bin/sla-slicer)
#   bin/sla-slicer    real binary; resources are found at <exe>/../resources (non-FHS build)
#   resources/            installed by `cmake --install` (no udev rules)
#   lib/                  non-host-ABI native libraries (OpenCV, TIFF, ...) from tools/bundle_native_libraries.py
#
# Requirements: the build must be configured WITHOUT -DSLIC3R_FHS=1 (the default), otherwise
# resources are looked up in an absolute share dir and the bundle is not relocatable.
# Host-provided libraries (glibc, GTK/GL/X11/Wayland stack, libstdc++) are deliberately NOT bundled;
# the target machine needs a compatible GTK3 + OpenGL installation.
#
# BUNDLE_LIBSTDCXX=1 additionally keeps libstdc++.so.6 and libgcc_s.so.1 in lib/ (use it when the binary was
# built with a newer GCC than the oldest target distribution ships; see tools/docker/build_portable.sh).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"
# Product version (SLA_SLICER_VERSION), not the PrusaSlicer compatibility version SLIC3R_VERSION.
VERSION="$(sed -n 's/^set(SLA_SLICER_VERSION "\(.*\)")/\1/p' "${ROOT_DIR}/version.inc")"
[[ -n "$VERSION" ]] || { echo "SLA_SLICER_VERSION not found in version.inc" >&2; exit 2; }
NAME="sla-slicer-${VERSION}"
STAGE="${ROOT_DIR}/dist/${NAME}"   # bundle_native_libraries.py insists on a project-local output

if [[ ! -x "${BUILD_DIR}/src/sla-slicer" ]]; then
  echo "Build first (bash ./build_linux.sh); missing ${BUILD_DIR}/src/sla-slicer" >&2
  exit 2
fi
if [[ -e "$STAGE" ]]; then
  echo "Refusing to overwrite existing $STAGE (remove it first)" >&2
  exit 2
fi

# 1. Install into the stage: bin/ + resources/ (prefix layout, not FHS).
cmake --install "$BUILD_DIR" --prefix "$STAGE"

# 2. Collect the native dependency closure, then drop libraries that must come from the host.
#    Besides glibc and the GTK/GL/X11/Wayland stack, every library that stack itself depends on (GLib's
#    libmount/libblkid/libselinux/libpcre/libffi, systemd, compression, libpng, expat, ...) stays on the
#    host: those keep backward-compatible sonames, and an older bundled copy shadows the newer one the host's
#    GTK needs (verified: bundling Ubuntu 22.04's libmount broke Debian testing, Fedora and Arch).
#    Only application-specific libraries remain bundled (OpenCV, TBB, TIFF + jbig/deflate, OpenJPEG) plus libjpeg and libwebp:
#    libjpeg's soname differs between distributions (libjpeg.so.8 vs Fedora's libjpeg.so.62) and libwebp.so.7
#    is not part of every base install (Debian testing); the bundled libtiff needs both.
python3 "${ROOT_DIR}/tools/bundle_native_libraries.py" "${BUILD_DIR}/src/sla-slicer" "${STAGE}/lib"
if [[ "${BUNDLE_LIBSTDCXX:-0}" == 1 ]]; then CXXRT=''; else CXXRT='libstdc\+\+\.|libgcc_s\.|'; fi
HOST_LIBS='^(ld-linux.*|libc\.|libm\.|libdl\.|libpthread\.|librt\.|libresolv\.|libutil\.|'"${CXXRT}"'libGL|libEGL|libGLX|libGLU|libOpenGL|libX|libxcb|libxkb|libwayland|libgtk|libgdk|libgio|libglib|libgobject|libgmodule|libpango|libcairo|libatk|libharfbuzz|libfontconfig|libfreetype|libdbus|libSM\.|libICE\.|libnotify|libgdk_pixbuf|'\
'libatspi|libepoxy|libpixman|libfribidi|libthai|libdatrie|libgraphite2|libbrotli|libexpat|libffi|libpcre|libmount|libblkid|'\
'libselinux|libsystemd|libudev|libcap\.|libgcrypt|libgpg-error|liblz4|liblzma|libzstd|libz\.|libbsd|libmd\.|libuuid|'\
'libpng16)'
find "${STAGE}/lib" -maxdepth 1 -type f \
  | while read -r f; do
      if [[ "$(basename "$f")" =~ $HOST_LIBS ]]; then rm -f "$f"; fi
    done

# 3. Launcher independent of the current directory / install location.
cat > "${STAGE}/sla-slicer" <<'EOF'
#!/usr/bin/env bash
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
export LD_LIBRARY_PATH="${HERE}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${HERE}/bin/sla-slicer" "$@"
EOF
chmod +x "${STAGE}/sla-slicer"

# 4. Tarball.
tar -C "${ROOT_DIR}/dist" -czf "${ROOT_DIR}/dist/${NAME}-linux-x86_64.tar.gz" "$NAME"
echo "Created ${ROOT_DIR}/dist/${NAME}-linux-x86_64.tar.gz"
echo "Verify (no network libraries): readelf -d ${STAGE}/bin/sla-slicer | grep NEEDED"
