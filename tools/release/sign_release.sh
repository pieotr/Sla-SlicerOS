#!/usr/bin/env bash
# Checksums and (optional) detached GPG signatures for a release directory.
#
# Usage: tools/release/sign_release.sh [--per-file] DIR [GPG_KEY_ID]
#   DIR         directory holding the release artefacts (regular files, no sub-directories are descended)
#   GPG_KEY_ID  key used for signing; default: the environment variable SLA_SLICER_GPG_KEY. Honours GNUPGHOME.
#   --per-file  additionally write <artefact>.asc for every artefact (default: only SHA256SUMS.asc)
# Writes DIR/SHA256SUMS in `sha256sum` format (sorted by name, covering every regular file in DIR except SHA256SUMS,
# SHA256SUMS.asc and *.asc) and, with a key, the detached ASCII-armoured signature DIR/SHA256SUMS.asc.
# Without a key it prints how to sign later and exits 0.
# Users verify with:  sha256sum -c SHA256SUMS   and   gpg --verify SHA256SUMS.asc SHA256SUMS
set -euo pipefail
die() { echo "ABORT: $*" >&2; exit 1; }
PER_FILE=0
if [[ "${1:-}" == --per-file ]]; then PER_FILE=1; shift; fi
[[ $# -ge 1 && $# -le 2 && -d "$1" ]] || { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | cut -c3-; exit 2; }
DIR="$(cd "$1" && pwd)"; KEY="${2:-${SLA_SLICER_GPG_KEY:-}}"
cd "$DIR"
mapfile -d '' FILES < <(find . -maxdepth 1 -type f ! -name 'SHA256SUMS' ! -name 'SHA256SUMS.asc' ! -name '*.asc' ! -name '.*' -printf '%f\0' | LC_ALL=C sort -z)
[[ ${#FILES[@]} -gt 0 ]] || die "no release artefacts in $DIR"
sha256sum -- "${FILES[@]}" >SHA256SUMS.tmp && mv SHA256SUMS.tmp SHA256SUMS
echo "wrote $DIR/SHA256SUMS (${#FILES[@]} files)"
rm -f SHA256SUMS.asc
if [[ -z "$KEY" ]]; then
  cat <<EOT
No GPG key given (argument 2 or SLA_SLICER_GPG_KEY): SHA256SUMS is NOT signed. To sign later:
  gpg --local-user <KEY_ID> --armor --detach-sign --output $DIR/SHA256SUMS.asc $DIR/SHA256SUMS
  (or re-run: tools/release/sign_release.sh $DIR <KEY_ID>)
EOT
  exit 0
fi
command -v gpg >/dev/null || die "gpg not found"
gpg --batch --yes --local-user "$KEY" --armor --detach-sign --output SHA256SUMS.asc SHA256SUMS || die "signing SHA256SUMS failed"
echo "wrote $DIR/SHA256SUMS.asc (key $KEY)"
if [[ $PER_FILE -eq 1 ]]; then
  for f in "${FILES[@]}"; do
    gpg --batch --yes --local-user "$KEY" --armor --detach-sign --output "$f.asc" "$f" || die "signing $f failed"
  done
  echo "wrote ${#FILES[@]} per-file signatures (*.asc)"
fi
gpg --batch --verify SHA256SUMS.asc SHA256SUMS 2>&1 | sed 's/^/gpg: /;s/^gpg: gpg: /gpg: /' || true
