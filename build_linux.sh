#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_BUILD_DIR="${ROOT_DIR}/deps/build"
APP_BUILD_DIR="${ROOT_DIR}/build"
PREFIX_PATH="${DEPS_BUILD_DIR}/destdir/usr/local"
MIN_OPENCV="4.7"

usage() {
  cat <<'EOF'
Usage: ./build_linux.sh [--run] [--tests] [--jobs N] [--clean] [--deps]
                        [--bundled-opencv] [--check] [--skip-checks] [--docker]

Builds SLA-Slicer with the bundled dependency superbuild (deps/) and the application
(build/src/sla-slicer) on Linux (GTK3, static dependencies). Missing build tools and
development libraries are reported up front with install hints for your distribution.

Options:
  --run             Run a quick smoke test after build (sla-slicer --help)
  --tests           Build the SLA unit tests and run the smoke CTest subset
  --jobs N          Number of parallel jobs for the app build (default: nproc)
  --clean           Remove build and deps/build directories before configuring
  --deps            Force a dependency rebuild before the app build
  --bundled-opencv  Build OpenCV 4.12.0 (core, imgproc) with the dependencies instead of
                    using the system OpenCV. Needed when the system has no OpenCV >= 4.7
                    (Debian 12, Ubuntu 22.04/24.04 ship older versions).
  --check           Only check the prerequisites and exit
  --skip-checks     Do not check the prerequisites
  --docker          Build the portable tarball (Ubuntu 22.04 container, needs docker and
                    network) with tools/docker/build_portable.sh; result in build-docker/dist.
                    JOBS=N in the environment sets the parallelism.
  -h, --help        Show this help

Default behavior is optimized for incremental rebuild speed. See install.md for details.
EOF
}

RUN_AFTER_BUILD=0
JOBS="$(nproc)"
CLEAN=0
BUILD_DEPS=0
BUILD_TESTS=OFF
BUNDLED_OPENCV=0
CHECK_ONLY=0
SKIP_CHECKS=0
USE_DOCKER=0

opencv_in_prefix() {
  compgen -G "${PREFIX_PATH}/lib/cmake/opencv4/OpenCVConfig.cmake" >/dev/null
}

deps_ready() {
  [[ -f "${PREFIX_PATH}/lib/libwx_gtk3u_core-3.2.a" ]] || return 1
  if [[ "$BUNDLED_OPENCV" -eq 1 ]]; then opencv_in_prefix || return 1; fi
  return 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run)
      RUN_AFTER_BUILD=1
      shift
      ;;
    --tests)
      BUILD_TESTS=ON
      shift
      ;;
    --jobs)
      JOBS="${2:-}"
      if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
        echo "--jobs requires a positive integer" >&2
        exit 1
      fi
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --deps)
      BUILD_DEPS=1
      shift
      ;;
    --bundled-opencv)
      BUNDLED_OPENCV=1
      shift
      ;;
    --check)
      CHECK_ONLY=1
      shift
      ;;
    --skip-checks)
      SKIP_CHECKS=1
      shift
      ;;
    --docker)
      USE_DOCKER=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$USE_DOCKER" -eq 1 ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "--docker needs docker (not found in PATH)." >&2
    exit 1
  fi
  echo "Portable build in a container (first run: hours; later runs reuse build-docker/)"
  JOBS="$JOBS" exec bash "$ROOT_DIR/tools/docker/build_portable.sh"
fi

# ---------------------------------------------------------------------------
# Prerequisite check
# ---------------------------------------------------------------------------
detect_family() {
  local id="" like=""
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    id="$(. /etc/os-release; echo "${ID:-}")"
    like="$(. /etc/os-release; echo "${ID_LIKE:-}")"
  fi
  case " $id $like " in
    *" debian "*|*" ubuntu "*) echo debian ;;
    *" fedora "*|*" rhel "*|*" centos "*) echo fedora ;;
    *" arch "*|*" archlinux "*) echo arch ;;
    *) echo unknown ;;
  esac
}

# pkg_for <family> <key> -> distro package name (empty when unknown)
pkg_for() {
  local fam="$1" key="$2"
  case "$fam:$key" in
    debian:cmake) echo cmake;;           fedora:cmake) echo cmake;;            arch:cmake) echo cmake;;
    debian:git) echo git;;               fedora:git) echo git;;                arch:git) echo git;;
    debian:gcc) echo gcc;;               fedora:gcc) echo gcc;;                arch:gcc) echo gcc;;
    debian:g++) echo g++;;               fedora:g++) echo gcc-c++;;            arch:g++) echo gcc;;
    debian:make) echo make;;             fedora:make) echo make;;              arch:make) echo make;;
    debian:pkg-config) echo pkg-config;; fedora:pkg-config) echo pkgconf-pkg-config;; arch:pkg-config) echo pkgconf;;
    debian:makeinfo) echo texinfo;;      fedora:makeinfo) echo texinfo;;       arch:makeinfo) echo texinfo;;
    debian:m4) echo m4;;                 fedora:m4) echo m4;;                  arch:m4) echo m4;;
    debian:autoreconf) echo autoconf;;   fedora:autoreconf) echo autoconf;;    arch:autoreconf) echo autoconf;;
    debian:automake) echo automake;;     fedora:automake) echo automake;;      arch:automake) echo automake;;
    debian:libtoolize) echo libtool;;    fedora:libtoolize) echo libtool;;     arch:libtoolize) echo libtool;;
    debian:patch) echo patch;;           fedora:patch) echo patch;;            arch:patch) echo patch;;
    debian:python3) echo python3;;       fedora:python3) echo python3;;        arch:python3) echo python;;
    debian:gtk+-3.0) echo libgtk-3-dev;;     fedora:gtk+-3.0) echo gtk3-devel;;          arch:gtk+-3.0) echo gtk3;;
    debian:dbus-1) echo libdbus-1-dev;;      fedora:dbus-1) echo dbus-devel;;            arch:dbus-1) echo dbus;;
    debian:gl) echo libgl1-mesa-dev;;        fedora:gl) echo mesa-libGL-devel;;          arch:gl) echo libglvnd;;
    debian:glu) echo libglu1-mesa-dev;;      fedora:glu) echo mesa-libGLU-devel;;        arch:glu) echo glu;;
    debian:libtiff-4) echo libtiff-dev;;     fedora:libtiff-4) echo libtiff-devel;;      arch:libtiff-4) echo libtiff;;
    debian:libpng) echo libpng-dev;;         fedora:libpng) echo libpng-devel;;          arch:libpng) echo libpng;;
    debian:libjpeg) echo libjpeg-dev;;       fedora:libjpeg) echo libjpeg-turbo-devel;;  arch:libjpeg) echo libjpeg-turbo;;
    debian:libopenjp2) echo libopenjp2-7-dev;; fedora:libopenjp2) echo openjpeg2-devel;; arch:libopenjp2) echo openjpeg2;;
    debian:zlib) echo zlib1g-dev;;           fedora:zlib) echo zlib-devel;;              arch:zlib) echo zlib;;
    debian:x11) echo libx11-dev;;            fedora:x11) echo libX11-devel;;             arch:x11) echo libx11;;
    debian:sm) echo libsm-dev;;              fedora:sm) echo libSM-devel;;               arch:sm) echo libsm;;
    debian:ice) echo libice-dev;;            fedora:ice) echo libICE-devel;;             arch:ice) echo libice;;
    debian:libnotify) echo libnotify-dev;;   fedora:libnotify) echo libnotify-devel;;    arch:libnotify) echo libnotify;;
    debian:xkbcommon) echo libxkbcommon-dev;; fedora:xkbcommon) echo libxkbcommon-devel;; arch:xkbcommon) echo libxkbcommon;;
    debian:opencv) echo libopencv-dev;;      fedora:opencv) echo opencv-devel;;          arch:opencv) echo opencv;;
    *) echo "";;
  esac
}

install_cmd() {
  case "$1" in
    debian) echo "sudo apt-get install -y";;
    fedora) echo "sudo dnf install -y";;
    arch)   echo "sudo pacman -S --needed";;
    *)      echo "";;
  esac
}

# Highest OpenCV version found in the deps prefix (it wins in CMake) or in the system. Empty when none.
opencv_version() {
  local f v best=""
  local -a candidates=()
  if [[ -n "${OpenCV_DIR:-}" ]]; then candidates+=("${OpenCV_DIR}/OpenCVConfig-version.cmake"); fi
  if opencv_in_prefix; then
    # CMAKE_PREFIX_PATH makes find_package pick the dependency prefix first.
    candidates=("${PREFIX_PATH}/lib/cmake/opencv4/OpenCVConfig-version.cmake")
  else
    local d
    for d in /usr/lib*/cmake/opencv* /usr/lib/*/cmake/opencv* /usr/local/lib*/cmake/opencv* /usr/share/OpenCV /usr/share/opencv*; do
      candidates+=("$d/OpenCVConfig-version.cmake")
    done
  fi
  for f in "${candidates[@]}"; do
    [[ -r "$f" ]] || continue
    v="$(sed -n 's/^[Ss][Ee][Tt](OpenCV_VERSION \([0-9][0-9.]*\)).*/\1/p' "$f" | head -n1)"
    [[ -n "$v" ]] || continue
    if [[ -z "$best" || "$(printf '%s\n%s\n' "$best" "$v" | sort -V | tail -n1)" == "$v" ]]; then best="$v"; fi
  done
  echo "$best"
}

version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]; }

check_prerequisites() {
  local fam inst missing_pkgs=() problems=0
  fam="$(detect_family)"
  inst="$(install_cmd "$fam")"
  local -a miss_tools=() miss_libs=() warn_libs=()
  local t
  for t in cmake git gcc g++ make pkg-config makeinfo m4 autoreconf automake libtoolize patch; do
    command -v "$t" >/dev/null 2>&1 || miss_tools+=("$t")
  done
  if command -v cmake >/dev/null 2>&1; then
    local cv; cv="$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')"
    if ! version_ge "$cv" "3.16"; then
      echo "ERROR: CMake $cv is too old (>= 3.16 required, >= 3.28 recommended for the dependency build)." >&2
      problems=1
    fi
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    local l
    for l in gtk+-3.0 dbus-1 gl glu libtiff-4 libpng libjpeg libopenjp2 zlib x11; do
      pkg-config --exists "$l" 2>/dev/null || miss_libs+=("$l")
    done
    for l in sm ice libnotify xkbcommon; do
      pkg-config --exists "$l" 2>/dev/null || warn_libs+=("$l")
    done
  fi

  local k p
  for k in "${miss_tools[@]}" "${miss_libs[@]}"; do
    p="$(pkg_for "$fam" "$k")"
    [[ -n "$p" ]] && missing_pkgs+=("$p")
  done
  if [[ ${#miss_tools[@]} -gt 0 ]]; then echo "ERROR: missing tools: ${miss_tools[*]}" >&2; problems=1; fi
  if [[ ${#miss_libs[@]} -gt 0 ]]; then echo "ERROR: missing development libraries (pkg-config names): ${miss_libs[*]}" >&2; problems=1; fi
  if [[ ${#warn_libs[@]} -gt 0 ]]; then
    local wp=()
    for k in "${warn_libs[@]}"; do p="$(pkg_for "$fam" "$k")"; [[ -n "$p" ]] && wp+=("$p"); done
    echo "WARNING: optional development libraries not found (wxWidgets may build without some features): ${warn_libs[*]}" >&2
    [[ -n "$inst" && ${#wp[@]} -gt 0 ]] && echo "         optional packages: ${wp[*]}" >&2
  fi

  # OpenCV >= 4.7 (core, imgproc): system, deps prefix, or --bundled-opencv.
  if [[ "$BUNDLED_OPENCV" -eq 1 ]]; then
    echo "OpenCV: will be built with the dependencies (--bundled-opencv, version 4.12.0)."
  else
    local ov; ov="$(opencv_version)"
    if [[ -z "$ov" ]]; then
      echo "ERROR: OpenCV development files not found (OpenCV >= ${MIN_OPENCV} is required)." >&2
      p="$(pkg_for "$fam" opencv)"
      [[ -n "$p" ]] && echo "       Install the system package (${p}) if your distribution ships OpenCV >= ${MIN_OPENCV}," >&2
      echo "       or re-run with --bundled-opencv to build OpenCV 4.12.0 together with the dependencies." >&2
      problems=1
    elif ! version_ge "$ov" "$MIN_OPENCV"; then
      echo "ERROR: OpenCV $ov found, but >= ${MIN_OPENCV} is required (cv::stackBlur). Re-run with --bundled-opencv." >&2
      problems=1
    else
      echo "OpenCV: $ov (>= ${MIN_OPENCV})"
    fi
  fi

  if [[ "$problems" -ne 0 ]]; then
    if [[ ${#missing_pkgs[@]} -gt 0 ]]; then
      echo >&2
      if [[ -n "$inst" ]]; then
        echo "Install the missing packages for your distribution (${fam}):" >&2
        echo "  ${inst} ${missing_pkgs[*]}" >&2
      fi
    fi
    if [[ "$fam" == unknown ]]; then
      echo "Distribution not recognized from /etc/os-release; see install.md for package names." >&2
    fi
    echo "Full list of prerequisites: install.md" >&2
    return 1
  fi
  echo "Prerequisites OK (${fam})."
  return 0
}

if [[ "$SKIP_CHECKS" -eq 0 ]]; then
  check_prerequisites || exit 1
fi
if [[ "$CHECK_ONLY" -eq 1 ]]; then
  exit 0
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [[ "$CLEAN" -eq 1 ]]; then
  echo "[1/5] Cleaning previous build directories"
  rm -rf "$DEPS_BUILD_DIR" "$APP_BUILD_DIR"
fi

if [[ "$BUILD_DEPS" -eq 1 ]] || ! deps_ready; then
  echo "[2/5] Configuring dependencies"
  DEPS_OPTS=(-DDEP_WX_GTK3=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
  if [[ "$BUNDLED_OPENCV" -eq 1 ]]; then DEPS_OPTS+=(-DDEP_OpenCV=ON); fi
  cmake -S "$ROOT_DIR/deps" -B "$DEPS_BUILD_DIR" "${DEPS_OPTS[@]}"

  echo "[3/5] Building dependencies (top-level serial, inner builds parallelized by deps scripts)"
  cmake --build "$DEPS_BUILD_DIR" -- -j1
else
  echo "[2/5] Dependencies already built (use --deps to rebuild)"
  echo "[3/5] Skipping dependency rebuild"
fi

echo "[4/5] Configuring SLA-Slicer"
cmake -S "$ROOT_DIR" -B "$APP_BUILD_DIR" \
  -DSLIC3R_STATIC=1 \
  -DSLIC3R_GTK=3 \
  -DSLIC3R_PCH=OFF \
  -DSLIC3R_BUILD_TESTS="$BUILD_TESTS" \
  '-DSLIC3R_TEST_SUITES=libslic3r;arrange;sla_print;thumbnails' \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_PREFIX_PATH="$PREFIX_PATH"

echo "[5/5] Building SLA-Slicer target (incremental)"
cmake --build "$APP_BUILD_DIR" --target PrusaSlicer -- -j"$JOBS"

if [[ "$BUILD_TESTS" == ON ]]; then
  cmake --build "$APP_BUILD_DIR" --target sla_print_tests -- -j"$JOBS"
  (cd "$APP_BUILD_DIR" && ctest --output-on-failure -L smoke)
fi

BIN="$APP_BUILD_DIR/src/sla-slicer"
if [[ ! -x "$BIN" ]]; then
  echo "Build finished, but binary not found at: $BIN" >&2
  exit 2
fi

if [[ "$RUN_AFTER_BUILD" -eq 1 ]]; then
  echo "Running smoke test: $BIN --help"
  "$BIN" --help >/dev/null
  echo "Smoke test passed"
fi

echo "Build successful"
echo "Run: $BIN"
