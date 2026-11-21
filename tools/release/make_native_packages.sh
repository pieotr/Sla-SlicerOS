#!/usr/bin/env bash
# Native Linux packages built from the relocatable portable bundle (tools/docker/build_portable.sh output).
#
# Usage: tools/release/make_native_packages.sh BUNDLE_DIR_OR_TARBALL OUT_DIR
#   BUNDLE_DIR_OR_TARBALL  dist/sla-slicer-<ver>/ (or an unpacked bundle) or sla-slicer-<ver>-linux-x86_64.tar.gz
#   OUT_DIR                receives SLA-Slicer-<ver>-x86_64.AppImage, sla-slicer_<ver>_amd64.deb,
#                          sla-slicer-<ver>-1.x86_64.rpm, SHA256SUMS, lintian.txt (stage/ and tools/ are work dirs)
# Build-time downloads only (the application itself stays offline): appimagetool 1.9.1 and the type2 AppImage runtime
# (SHA256 pinned below, cached in OUT_DIR/tools), docker images ubuntu:22.04 (dpkg-deb + lintian) and fedora:latest (rpmbuild).
# Layout of .deb/.rpm: /opt/sla-slicer (the bundle), /usr/bin/sla-slicer -> /opt/sla-slicer/sla-slicer (the launcher resolves
# symlinks), .desktop (io.github.pieotr.SLASlicer.desktop) in /usr/share/applications, AppStream metainfo in /usr/share/metainfo, man page in /usr/share/man/man1, icons in /usr/share/icons/hicolor, licenses + SBOM (manifest.json) in
# /usr/share/doc/sla-slicer. The system must provide GTK3, OpenGL, dbus, libSM/libICE, libnotify (everything else is bundled).
# Env: NICE (default "nice -n 10"). Needs a running docker daemon.
set -euo pipefail
[[ $# -eq 2 ]] || { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | cut -c3-; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IN="$1"; mkdir -p "$2"; OUT="$(cd "$2" && pwd)"
NICE="${NICE-nice -n 10}"
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
RUNTIME_SHA=617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46
die() { echo "ABORT: $*" >&2; exit 1; }
command -v docker >/dev/null && docker info >/dev/null 2>&1 || die "docker daemon unreachable"

# ---- resolve the bundle ----
if [[ -f "$IN" ]]; then
  mkdir -p "$OUT/unpack"; rm -rf "$OUT/unpack"/*; tar -xzf "$IN" -C "$OUT/unpack"
  BUNDLE="$(echo "$OUT"/unpack/*/)"
else BUNDLE="$IN"; fi
BUNDLE="$(cd "$BUNDLE" && pwd)"
[[ -x "$BUNDLE/sla-slicer" && -x "$BUNDLE/bin/sla-slicer" ]] || die "$BUNDLE is not a bundle (sla-slicer, bin/sla-slicer)"
VER="$(basename "$BUNDLE")"; VER="${VER#sla-slicer-}"; [[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || VER="$(sed -n 's/^set(SLA_SLICER_VERSION "\(.*\)")/\1/p' "$ROOT/version.inc")"
ICONS="$BUNDLE/resources/icons"
for f in SLA-Slicer_16px.png SLA-Slicer_32px.png SLA-Slicer_48px.png SLA-Slicer_128px.png SLA-Slicer_192px.png SLA-Slicer.png SLA-Slicer_512px.png SLA-Slicer.svg; do [[ -f "$ICONS/$f" ]] || die "missing icon $f"; done
SRC_DATE="${SOURCE_DATE_EPOCH:-$(date +%s)}"

# ---- shared metadata ----
DESKTOP_BODY="[Desktop Entry]
Name=SLA-Slicer
GenericName=SLA 3D Printing Software
Comment=Offline slicer for SLA/MSLA resin printers
Icon=sla-slicer
Exec=sla-slicer %F
Terminal=false
Type=Application
MimeType=model/stl;application/vnd.ms-3mfdocument;application/prs.wavefront-obj;application/x-amf;
Categories=Graphics;3DGraphics;Engineering;
Keywords=3D;Printing;Slicer;slice;printer;convert;stl;obj;amf;SLA;MSLA;resin;
StartupNotify=false
StartupWMClass=sla-slicer"
put_icons() {  # $1 = root of a prefix that has share/icons
  local d
  for s in 16:SLA-Slicer_16px.png 32:SLA-Slicer_32px.png 48:SLA-Slicer_48px.png 128:SLA-Slicer_128px.png 192:SLA-Slicer_192px.png 256:SLA-Slicer.png 512:SLA-Slicer_512px.png; do
    d="$1/share/icons/hicolor/${s%%:*}x${s%%:*}/apps"; mkdir -p "$d"; cp "$ICONS/${s#*:}" "$d/sla-slicer.png"
  done
  d="$1/share/icons/hicolor/scalable/apps"; mkdir -p "$d"; cp "$ICONS/SLA-Slicer.svg" "$d/sla-slicer.svg"
}
# AppStream metainfo and man page live in the source tree; the desktop file is named after the AppStream component id.
APP_ID=io.github.pieotr.SLASlicer
METAINFO="$ROOT/src/platform/unix/$APP_ID.metainfo.xml"; MANPAGE="$ROOT/src/platform/unix/sla-slicer.1"
[[ -f "$METAINFO" && -f "$MANPAGE" ]] || die "missing $METAINFO or $MANPAGE"
grep -q "<release version=\"$VER\"" "$METAINFO" || die "$METAINFO has no <release version=\"$VER\">"

# ---- stage tree shared by .deb and .rpm ----
ST="$OUT/stage"; rm -rf "$ST"; mkdir -p "$ST/opt" "$ST/usr/bin" "$ST/usr/share/applications" "$ST/usr/share/doc/sla-slicer" "$ST/usr/share/metainfo" "$ST/usr/share/man/man1"
cp -a "$BUNDLE" "$ST/opt/sla-slicer"
ln -s /opt/sla-slicer/sla-slicer "$ST/usr/bin/sla-slicer"
printf '%s\n' "$DESKTOP_BODY" >"$ST/usr/share/applications/$APP_ID.desktop"
cp "$METAINFO" "$ST/usr/share/metainfo/$APP_ID.metainfo.xml"
gzip -9n -c "$MANPAGE" >"$ST/usr/share/man/man1/sla-slicer.1.gz"
put_icons "$ST/usr"
cp "$ROOT/LICENSE" "$ST/usr/share/doc/sla-slicer/LICENSE"
[[ -f "$BUNDLE/lib/manifest.json" ]] && cp "$BUNDLE/lib/manifest.json" "$ST/usr/share/doc/sla-slicer/sbom-manifest.json"
cp -a "$BUNDLE/lib/licenses" "$ST/usr/share/doc/sla-slicer/third-party-licenses"
find "$ST" -type d -exec chmod 755 {} +; find "$ST" -type f \( -perm -u+x \) -exec chmod 755 {} +; find "$ST" -type f ! -perm -u+x -exec chmod 644 {} +
find "$ST" -type f -name '*.so*' -exec chmod 644 {} + ; chmod 755 "$ST/opt/sla-slicer/bin/OCCTWrapper.so" 2>/dev/null || true
find "$ST" -type f -name '*.so*' -exec chmod 644 {} +   # shared objects need not be executable (lintian shlib-with-executable-bit)
chmod 644 "$ST/opt/sla-slicer/bin/OCCTWrapper.so"

# ---- AppImage ----
TOOLS="$OUT/tools"; mkdir -p "$TOOLS"
fetch() { # url sha file
  if [[ ! -f "$3" ]] || [[ "$(sha256sum "$3" | cut -d' ' -f1)" != "$2" ]]; then curl -fsSL -o "$3" "$1"; fi
  [[ "$(sha256sum "$3" | cut -d' ' -f1)" == "$2" ]] || die "SHA256 mismatch for $3"; chmod +x "$3"
}
fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$TOOLS/appimagetool-x86_64.AppImage"
fetch "$RUNTIME_URL" "$RUNTIME_SHA" "$TOOLS/runtime-x86_64"
AD="$OUT/AppDir"; rm -rf "$AD"; mkdir -p "$AD/usr/share/metainfo"
cp -a "$BUNDLE" "$AD/sla-slicer"
mkdir -p "$AD/sla-slicer/licenses"; cp "$ROOT/LICENSE" "$AD/sla-slicer/licenses/LICENSE"
cat >"$AD/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/sla-slicer/sla-slicer" "$@"
RUN
chmod 755 "$AD/AppRun"
printf '%s\n' "$DESKTOP_BODY" >"$AD/$APP_ID.desktop"
cp "$ICONS/SLA-Slicer_192px.png" "$AD/sla-slicer.png"; cp "$ICONS/SLA-Slicer_192px.png" "$AD/.DirIcon"
cp "$METAINFO" "$AD/usr/share/metainfo/$APP_ID.metainfo.xml"
find "$AD" -type d -exec chmod 755 {} +
APPIMG="$OUT/SLA-Slicer-$VER-x86_64.AppImage"; rm -f "$APPIMG"
ARCH=x86_64 $NICE "$TOOLS/appimagetool-x86_64.AppImage" --appimage-extract-and-run --no-appstream --runtime-file "$TOOLS/runtime-x86_64" \
  "$AD" "$APPIMG" || die "appimagetool failed"
chmod 755 "$APPIMG"; rm -rf "$AD"

# ---- .deb (ubuntu:22.04: dpkg-deb + lintian) ----
DEB="sla-slicer_${VER}_amd64.deb"
DEB_INNER='
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null; apt-get install -y -qq dpkg-dev lintian xz-utils gzip >/dev/null
P=/tmp/pkg; mkdir -p $P/DEBIAN; cp -a /stage/. $P/
cat >$P/usr/share/doc/sla-slicer/copyright <<CP
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: SLA-Slicer (fork of PrusaSlicer)

Files: *
Copyright: 2011-2024 Prusa Research s.r.o., Alessandro Ranellucci, and contributors
License: AGPL-3.0-only
 The full license text is in /usr/share/doc/sla-slicer/LICENSE; bundled third-party
 libraries and their licenses are listed in /usr/share/doc/sla-slicer/third-party-licenses
 and sbom-manifest.json.
CP
printf "sla-slicer (%s-1) unstable; urgency=low\n\n  * Native package of the portable SLA-Slicer bundle.\n\n -- SLA-Slicer maintainers <noreply@example.invalid>  %s\n" "$VER" "$(date -R -d @$SRC_DATE)" | gzip -9n >$P/usr/share/doc/sla-slicer/changelog.Debian.gz
chmod 644 $P/usr/share/doc/sla-slicer/copyright
# installed size in KiB
SZ=$(du -sk --exclude=DEBIAN $P | cut -f1)
mkdir -p $P/usr/share/lintian/overrides
cat >$P/usr/share/lintian/overrides/sla-slicer <<OV
# Relocatable bundle: everything lives in /opt/sla-slicer by design (identical to the tested portable tarball).
sla-slicer: dir-or-file-in-opt
# Bundled, version-pinned libraries (TBB, OpenCV, libjpeg, libtiff, ...) are part of the verified bundle.
sla-slicer: embedded-library
# Binaries are shipped exactly as produced and tested by the portable build.
sla-slicer: unstripped-binary-or-object
sla-slicer: duplicate-font-file
sla-slicer: initial-upload-closes-no-bugs
OV
chmod 644 $P/usr/share/lintian/overrides/sla-slicer
cat >$P/DEBIAN/control <<CT
Package: sla-slicer
Version: $VER-1
Architecture: amd64
Maintainer: SLA-Slicer maintainers <noreply@example.invalid>
Installed-Size: $SZ
Section: graphics
Priority: optional
Depends: libc6 (>= 2.35), libstdc++6 (>= 12), libgtk-3-0 | libgtk-3-0t64, libgl1, libglu1-mesa | libglu1, libegl1, libdbus-1-3, libsm6, libice6, libnotify4
Recommends: libgl1-mesa-dri, adwaita-icon-theme
Homepage: https://github.com/pieotr/Sla-SlicerOS
Description: offline slicer for SLA/MSLA resin 3D printers
 SLA-Slicer is a PrusaSlicer 2.9.4 fork for resin printers with export to many
 MSLA archive formats. It runs fully offline and ships its own libraries under
 /opt/sla-slicer; only GTK3, OpenGL, dbus, libSM/libICE and libnotify come from
 the system.
CT
cat >$P/DEBIAN/postinst <<CT
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
  command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
CT
cp $P/DEBIAN/postinst $P/DEBIAN/postrm; sed -i "s/configure/remove/" $P/DEBIAN/postrm
chmod 755 $P/DEBIAN/postinst $P/DEBIAN/postrm
dpkg-deb --root-owner-group -Zxz -z6 --build $P /out/'"$DEB"' >/dev/null
lintian --info --tag-display-limit 0 /out/'"$DEB"' >/out/lintian.txt 2>&1 || true
chown '"$(id -u):$(id -g)"' /out/'"$DEB"' /out/lintian.txt
'
$NICE docker run --rm -e SRC_DATE="$SRC_DATE" -e VER="$VER" -v "$ST":/stage:ro -v "$OUT":/out ubuntu:22.04 bash -c "$DEB_INNER" || die "deb build failed"

# ---- .rpm (fedora:latest: rpmbuild) ----
RPM="sla-slicer-${VER}-1.x86_64.rpm"
RPM_INNER='
set -eu
dnf -y -q install rpm-build >/dev/null 2>&1
cat >/tmp/sla-slicer.spec <<SPEC
Name: sla-slicer
Version: $VER
Release: 1
Summary: Offline slicer for SLA/MSLA resin 3D printers
License: AGPL-3.0-only
URL: https://github.com/pieotr/Sla-SlicerOS
BuildArch: x86_64
AutoReqProv: no
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
%global _binary_payload w6.xzdio
Requires: glibc >= 2.35, libstdc++, gtk3, mesa-libGL, mesa-libGLU, mesa-libEGL, dbus-libs, libSM, libICE, libnotify
Recommends: mesa-dri-drivers, adwaita-icon-theme
%description
SLA-Slicer is a PrusaSlicer 2.9.4 fork for resin printers with export to many MSLA archive formats.
It runs fully offline and ships its own libraries under /opt/sla-slicer; only GTK3, OpenGL, dbus,
libSM/libICE and libnotify come from the system.
%install
mkdir -p %{buildroot}
cp -a /stage/. %{buildroot}/
%files
/opt/sla-slicer
/usr/bin/sla-slicer
/usr/share/applications/io.github.pieotr.SLASlicer.desktop
/usr/share/metainfo/io.github.pieotr.SLASlicer.metainfo.xml
/usr/share/man/man1/sla-slicer.1.gz
/usr/share/icons/hicolor/*/apps/sla-slicer.*
%dir /usr/share/doc/sla-slicer
%license /usr/share/doc/sla-slicer/LICENSE
%doc /usr/share/doc/sla-slicer/sbom-manifest.json
%doc /usr/share/doc/sla-slicer/third-party-licenses
%changelog
* Fri Nov 20 2026 SLA-Slicer maintainers <noreply@example.invalid> - $VER-1
- Native package of the portable SLA-Slicer bundle.
SPEC
rpmbuild -bb --define "_topdir /tmp/rpmtop" --define "_rpmdir /out" --define "_rpmfilename %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" /tmp/sla-slicer.spec >/tmp/rpmbuild.log 2>&1 || { tail -30 /tmp/rpmbuild.log; exit 1; }
chown '"$(id -u):$(id -g)"' /out/'"$RPM"'
'
$NICE docker run --rm -e VER="$VER" -v "$ST":/stage:ro -v "$OUT":/out fedora:latest bash -c "$RPM_INNER" || die "rpm build failed"

rm -rf "$OUT/unpack"
( cd "$OUT" && sha256sum "$(basename "$APPIMG")" "$DEB" "$RPM" >SHA256SUMS )
echo "== packages in $OUT"; ls -l "$APPIMG" "$OUT/$DEB" "$OUT/$RPM"; echo "== lintian:"; grep -E '^[EW]:' "$OUT/lintian.txt" | sort | uniq -c | sort -rn || true
