#!/usr/bin/env bash
# Manual mid-refactor testing with persistent, separate user profiles and a log.
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${APP_DIR}/build/mid-refactor-user-tests"
mkdir -p "${TEST_DIR}/presets"
printf 'Profile testowe: %s\nLog aplikacji: %s\n' "${TEST_DIR}/presets" "${TEST_DIR}/application.log"
exec bash "${APP_DIR}/run_prusa_slicer.sh" --datadir "${TEST_DIR}/presets" "$@" >> "${TEST_DIR}/application.log" 2>&1
