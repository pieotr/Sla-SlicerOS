#!/usr/bin/env bash
# Reproducible source tarball of exactly the repository file set.
#
# Usage: tools/release/make_source_tarball.sh OUT_DIR
#   writes OUT_DIR/sla-slicer-<SLA_SLICER_VERSION>-src.tar.gz (top directory sla-slicer-<ver>/) and prints its path + sha256.
#
# * file set = `git ls-files -co --exclude-standard` (tracked + untracked-but-not-ignored), files missing on disk skipped,
#   i.e. what a commit of the working tree would contain; local QA tooling that .gitignore excludes is NOT included;
# * deterministic: names sorted (C locale), mtime = SOURCE_DATE_EPOCH or the last commit time, owner/group 0/0 (numeric),
#   fixed permissions (644 / 755), tar --format=gnu, gzip -n; two runs on the same tree give the same sha256;
# * the archive carries a BUILD_ID file (first line = build id suffix as derived by cmake/SlaSlicerBuildId.cmake from
#   `git describe --always --dirty`, or the SLA_SLICER_BUILD_ID environment variable), so that a build from the tarball,
#   which has no .git, shows the same id: "SLA-Slicer-<ver>+<BUILD_ID>".
# Needs: git, GNU tar, gzip, sort, sha256sum.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[[ $# -eq 1 ]] || { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | cut -c3-; exit 2; }
die() { echo "ABORT: $*" >&2; exit 1; }
mkdir -p "$1"; OUT="$(cd "$1" && pwd)"
cd "$ROOT"
git rev-parse --git-dir >/dev/null 2>&1 || die "$ROOT is not a git checkout"
VER="$(sed -n 's/^set(SLA_SLICER_VERSION "\(.*\)")/\1/p' version.inc)"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+.][A-Za-z0-9.]+)?$ ]] || die "cannot read SLA_SLICER_VERSION from version.inc"
EPOCH="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || true)}"
[[ "$EPOCH" =~ ^[0-9]+$ ]] || die "no SOURCE_DATE_EPOCH and no commit to take the time from"
SUFFIX="${SLA_SLICER_BUILD_ID:-$(git describe --always --dirty 2>/dev/null || true)}"
[[ -n "$SUFFIX" ]] || SUFFIX=source
SUFFIX="$(printf '%s' "$SUFFIX" | tr -c 'A-Za-z0-9._\n-' '-')"   # same sanitising as SlaSlicerBuildId.cmake
NAME="sla-slicer-$VER"; TARBALL="$OUT/$NAME-src.tar.gz"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
printf '%s\n' "$SUFFIX" >"$TMP/BUILD_ID"
# NUL-separated, sorted, de-duplicated list; BUILD_ID is generated, never taken from the working tree.
# Tracked files that .gitignore now excludes (local QA, download caches) are left out too, so the set does
# not depend on whether `git rm --cached` has been run for them yet.
declare -A IGNORED_TRACKED=()
while IFS= read -r -d '' f; do IGNORED_TRACKED["$f"]=1; done < <(git ls-files -z -ci --exclude-standard)
git ls-files -co --exclude-standard -z | while IFS= read -r -d '' f; do
  [[ "$f" == BUILD_ID ]] && continue
  [[ -n "${IGNORED_TRACKED[$f]:-}" ]] && continue
  if [[ -e "$f" || -L "$f" ]]; then printf '%s\0' "$f"; fi
done | LC_ALL=C sort -z -u >"$TMP/files.list"
[[ -s "$TMP/files.list" ]] || die "empty file list"
# BUILD_ID goes first (as its absolute path inside $TMP, renamed by --transform below).
{ printf '%s\0' "$TMP/BUILD_ID"; cat "$TMP/files.list"; } >"$TMP/all.list"

tar --create --format=gnu --null --absolute-names --no-recursion --hard-dereference --files-from="$TMP/all.list" \
    --transform="s,^$TMP/BUILD_ID\$,BUILD_ID," --transform="s,^,$NAME/," \
    --owner=0 --group=0 --numeric-owner --mtime="@$EPOCH" --mode='u+rw,go+r,go-w' \
    --file=- | gzip -n -9 >"$TARBALL.tmp"
mv "$TARBALL.tmp" "$TARBALL"
echo "$TARBALL"
echo "files: $(( $(tr -cd '\0' <"$TMP/all.list" | wc -c) )) (incl. BUILD_ID=$SUFFIX), mtime epoch $EPOCH"
( cd "$OUT" && sha256sum "$(basename "$TARBALL")" )
