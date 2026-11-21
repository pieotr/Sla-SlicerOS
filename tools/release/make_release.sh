#!/usr/bin/env bash
# Assemble the complete release directory from the portable bundle tarball produced by the docker build.
#
# Usage: tools/release/make_release.sh BUNDLE_TARBALL OUT_DIR [GPG_KEY_ID]
#   BUNDLE_TARBALL  dist/sla-slicer-<ver>-linux-x86_64.tar.gz (tools/docker/build_portable.sh / build_linux.sh --docker)
#   OUT_DIR         e.g. dist/sla-slicer-<ver>/ ; receives
#                     sla-slicer-<ver>-linux-x86_64.tar.gz   the portable bundle (copied)
#                     SLA-Slicer-<ver>-x86_64.AppImage, sla-slicer_<ver>_amd64.deb, sla-slicer-<ver>-1.x86_64.rpm
#                                                            (tools/release/make_native_packages.sh; needs docker)
#                     sla-slicer-<ver>-src.tar.gz            (tools/release/make_source_tarball.sh)
#                     RELEASE_NOTES.md                       the <ver> section of CHANGELOG.md + install / verify instructions
#                     SHA256SUMS [+ SHA256SUMS.asc]          (tools/release/sign_release.sh; signature only with a key:
#                                                             3rd argument or SLA_SLICER_GPG_KEY)
# <ver> is SLA_SLICER_VERSION of version.inc; the tarball must be named for it (a stale bundle of another version is refused).
# Env: SLA_SLICER_RELEASE_WORK (work directory of the package build, default OUT_DIR.work; removed afterwards unless
#      SLA_SLICER_KEEP_WORK=1), plus everything the called scripts honour (NICE, SOURCE_DATE_EPOCH, SLA_SLICER_BUILD_ID).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/tools/release"
[[ $# -ge 2 && $# -le 3 ]] || { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | cut -c3-; exit 2; }
die() { echo "ABORT: $*" >&2; exit 1; }
[[ -f "$1" ]] || die "bundle tarball not found: $1"
BUNDLE_TAR="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
mkdir -p "$2"; OUT="$(cd "$2" && pwd)"; KEY="${3:-${SLA_SLICER_GPG_KEY:-}}"
VER="$(sed -n 's/^set(SLA_SLICER_VERSION "\(.*\)")/\1/p' "$ROOT/version.inc")"
[[ -n "$VER" ]] || die "cannot read SLA_SLICER_VERSION from version.inc"
BUNDLE_NAME="sla-slicer-$VER-linux-x86_64.tar.gz"
[[ "$(basename "$BUNDLE_TAR")" == "$BUNDLE_NAME" ]] || die "bundle must be named $BUNDLE_NAME (version.inc says $VER)"
tar -tzf "$BUNDLE_TAR" "sla-slicer-$VER/sla-slicer" >/dev/null 2>&1 || die "$BUNDLE_TAR does not contain sla-slicer-$VER/sla-slicer"
grep -q "^## $VER\b" "$ROOT/CHANGELOG.md" || die "CHANGELOG.md has no '## $VER' section"
WORK="${SLA_SLICER_RELEASE_WORK:-$OUT.work}"

echo "== release $VER -> $OUT"
[[ "$BUNDLE_TAR" -ef "$OUT/$BUNDLE_NAME" ]] || cp -f "$BUNDLE_TAR" "$OUT/$BUNDLE_NAME"

echo "== native packages (AppImage, .deb, .rpm)"
mkdir -p "$WORK"
bash "$HERE/make_native_packages.sh" "$BUNDLE_TAR" "$WORK"
for f in "SLA-Slicer-$VER-x86_64.AppImage" "sla-slicer_${VER}_amd64.deb" "sla-slicer-$VER-1.x86_64.rpm"; do
  [[ -f "$WORK/$f" ]] || die "make_native_packages.sh did not produce $f"
  cp -f "$WORK/$f" "$OUT/$f"
done

echo "== source tarball"
bash "$HERE/make_source_tarball.sh" "$OUT"

echo "== RELEASE_NOTES.md"
{
  echo "# SLA-Slicer $VER"
  echo
  awk -v ver="$VER" '
    $0 ~ "^## " ver "([^0-9.]|$)" { on = 1; next }
    on && /^## / { exit }
    on { print }' "$ROOT/CHANGELOG.md" | sed '/./,$!d' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
  cat <<EOT

## Instalacja

\`\`\`sh
chmod +x SLA-Slicer-$VER-x86_64.AppImage && ./SLA-Slicer-$VER-x86_64.AppImage     # AppImage (bez FUSE: --appimage-extract-and-run)
sudo apt install ./sla-slicer_${VER}_amd64.deb                                    # Debian, Ubuntu
sudo dnf install ./sla-slicer-$VER-1.x86_64.rpm                                   # Fedora
tar xzf $BUNDLE_NAME && ./sla-slicer-$VER/sla-slicer      # przenośne archiwum
\`\`\`

Wymagania: Linux x86_64, glibc 2.35 lub nowszy, GTK3, OpenGL, dbus, libSM/libICE, libnotify.
Interfejs: angielski i polski. Szczegóły: install.md w archiwum źródeł.

## Weryfikacja plików

\`\`\`sh
sha256sum -c SHA256SUMS                     # sumy kontrolne wszystkich plików wydania
gpg --verify SHA256SUMS.asc SHA256SUMS      # podpis (tylko wydania podpisane; wymaga klucza wydawcy)
\`\`\`

## Źródła

\`sla-slicer-$VER-src.tar.gz\` zawiera dokładnie zestaw plików repozytorium i plik \`BUILD_ID\`;
budowa z niego (bez \`.git\`) daje ten sam identyfikator budowy co budowa z repozytorium.
Licencja: AGPL-3.0 (plik LICENSE); licencje bibliotek zewnętrznych są w pakietach, w
\`/usr/share/doc/sla-slicer\`.
EOT
} >"$OUT/RELEASE_NOTES.md"

echo "== checksums / signature"
bash "$HERE/sign_release.sh" "$OUT" ${KEY:+"$KEY"}

[[ "${SLA_SLICER_KEEP_WORK:-0}" == 1 ]] || rm -rf "$WORK"
echo "== done: $OUT"; ls -l "$OUT"
