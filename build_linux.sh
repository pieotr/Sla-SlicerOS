#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_BUILD_DIR="${ROOT_DIR}/deps/build"
APP_BUILD_DIR="${ROOT_DIR}/build"
PREFIX_PATH="${DEPS_BUILD_DIR}/destdir/usr/local"

detect_ca_bundle() {
  local candidates=(
    "/etc/pki/tls/certs/ca-bundle.crt"
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"
    "/etc/ssl/certs/ca-certificates.crt"
    "/etc/ssl/certs/ca-bundle.crt"
    "/etc/ca-certificates/extracted/tls-ca-bundle.pem"
    "/usr/share/ssl/certs/ca-bundle.crt"
    "/usr/local/share/certs/ca-root-nss.crt"
    "/etc/ssl/certs/ca-root-nss.crt"
    "/etc/ssl/cert.pem"
    "/etc/ssl/ca-bundle.pem"
  )

  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

usage() {
  cat <<'EOF'
Usage: ./build_linux.sh [--run] [--jobs N] [--clean] [--deps]

Builds PrusaSlicer statically with bundled deps on Linux.
Default behavior is optimized for incremental rebuild speed.

Options:
  --run       Run a quick smoke test after build (prusa-slicer --help)
  --jobs N    Number of parallel jobs for app build (default: nproc)
  --clean     Remove build and deps/build directories before configuring
  --deps      Force dependency rebuild before app build
  -h, --help  Show this help
EOF
}

RUN_AFTER_BUILD=0
JOBS="$(nproc)"
CLEAN=0
BUILD_DEPS=0

deps_ready() {
  [[ -f "${PREFIX_PATH}/lib/libwx_gtk3u_core-3.2.a" ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run)
      RUN_AFTER_BUILD=1
      shift
      ;;
    --jobs)
      JOBS="${2:-}"
      if [[ -z "$JOBS" ]]; then
        echo "Missing value for --jobs" >&2
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

if [[ "$CLEAN" -eq 1 ]]; then
  echo "[1/5] Cleaning previous build directories"
  rm -rf "$DEPS_BUILD_DIR" "$APP_BUILD_DIR"
fi

if [[ "$BUILD_DEPS" -eq 1 ]] || ! deps_ready; then
  echo "[2/5] Configuring dependencies"
  cmake -S "$ROOT_DIR/deps" -B "$DEPS_BUILD_DIR" \
    -DDEP_WX_GTK3=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5

  echo "[3/5] Building dependencies (top-level serial, inner builds parallelized by deps scripts)"
  cmake --build "$DEPS_BUILD_DIR" -- -j1
else
  echo "[2/5] Dependencies already built (use --deps to rebuild)"
  echo "[3/5] Skipping dependency rebuild"
fi

echo "[4/5] Configuring PrusaSlicer"
cmake -S "$ROOT_DIR" -B "$APP_BUILD_DIR" \
  -DSLIC3R_STATIC=1 \
  -DSLIC3R_GTK=3 \
  -DSLIC3R_PCH=OFF \
  -DSLIC3R_SLA_ONLY=ON \
  -DSLIC3R_OFFLINE_ONLY=ON \
  -DSLIC3R_BUILD_TESTS=OFF \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_PREFIX_PATH="$PREFIX_PATH"

echo "[5/5] Building PrusaSlicer target (incremental)"
cmake --build "$APP_BUILD_DIR" --target PrusaSlicer -- -j"$JOBS"

BIN="$APP_BUILD_DIR/src/prusa-slicer"
if [[ ! -x "$BIN" ]]; then
  echo "Build finished, but binary not found at: $BIN" >&2
  exit 2
fi

if [[ "$RUN_AFTER_BUILD" -eq 1 ]]; then
  if ca_bundle="$(detect_ca_bundle)"; then
    export SSL_CERT_FILE="$ca_bundle"
    echo "Using SSL_CERT_FILE=$SSL_CERT_FILE"
  else
    echo "Warning: no system CA bundle detected; HTTPS features may fail" >&2
  fi

  echo "Running smoke test: $BIN --help"
  "$BIN" --help >/dev/null
  echo "Smoke test passed"
fi

echo "Build successful"
echo "Run: $BIN"
