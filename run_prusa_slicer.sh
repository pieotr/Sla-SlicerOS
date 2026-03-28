#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${ROOT_DIR}/build/src/prusa-slicer"

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

if [[ ! -x "$BIN" ]]; then
  echo "Binary not found: $BIN" >&2
  echo "Build first with: bash ./build_linux.sh" >&2
  exit 2
fi

if [[ -z "${SSL_CERT_FILE:-}" || ! -f "${SSL_CERT_FILE:-/does/not/exist}" ]]; then
  if ca_bundle="$(detect_ca_bundle)"; then
    export SSL_CERT_FILE="$ca_bundle"
  fi
fi

if [[ -n "${SSL_CERT_FILE:-}" ]]; then
  echo "Using SSL_CERT_FILE=$SSL_CERT_FILE"
else
  echo "Warning: SSL_CERT_FILE is not set and no CA bundle was auto-detected" >&2
fi

exec "$BIN" "$@"
