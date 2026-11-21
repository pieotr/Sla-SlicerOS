#!/usr/bin/env python3
"""Generate doc/SBOM.json (CycloneDX 1.5) for SLA-Slicer.

Sources of truth, in this order:
  * deps/+<Name>/*.cmake        -> upstream version of every statically built dependency
  * bundled_deps/, src/         -> in-tree third-party code (version read from its own headers/README)
  * build/src/CMakeFiles/PrusaSlicer.dir/link.txt -> which dependencies are REALLY linked
    (deps/ also builds Catch2 and JPEG/PNG that the offline SLA build never links)
  * ldd build/src/sla-slicer  -> system shared libraries needed at runtime
  * grep UVtools in src/        -> files that contain natively ported UVtools algorithms

Run from anywhere:  python3 -I tools/generate_sbom.py [--repo DIR] [--binary PATH] [--output FILE]
Nothing is downloaded and no git state is touched. Missing build artefacts only downgrade the
result ("linked" becomes "unknown"), they never fail the run, so the script also works on a
clean checkout. WHY a script and not a hand-written JSON: versions/links drift with every
dependency bump, and the license documentation (doc/THIRD_PARTY_LICENSES.md) must be checkable
against the real build.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

APP_NAME = "SLA-Slicer"
APP_LICENSE = "AGPL-3.0-only"  # effective license of the combined work (see THIRD_PARTY_LICENSES.md)

# ---------------------------------------------------------------------------------------------
# Inventory. Fields:
#   key        deps/+<key> directory name, bundled_deps/<key>, or an arbitrary id for in-tree parts
#   kind       deps | bundled | intree | resource
#   license    SPDX expression (verified against the LICENSE file shipped in the source tree)
#   linkage    static | header-only | plugin | not-linked | data
#   link_token substring that must occur in link.txt for the component to count as linked
#   notice     path (relative to repo) of the license text; for deps/ the path is relative to
#              deps/build/dep_<key>-prefix/src/dep_<key>/ and is resolved if the source exists
# ---------------------------------------------------------------------------------------------
DEPS = [
    dict(key="Boost", name="Boost", license="BSL-1.0", linkage="static", link_token="libboost_", notice="LICENSE_1_0.txt"),
    dict(key="TBB", name="oneTBB", license="Apache-2.0", linkage="static", link_token="libtbb.a", notice="LICENSE.txt"),
    dict(key="Eigen", name="Eigen", license="MPL-2.0", linkage="header-only", notice="COPYING.MPL2",
         note="Only MPL-2.0 code paths are used; Eigen also ships BSD/LGPL/GPL files that are not included."),
    dict(key="Cereal", name="cereal", license="BSD-3-Clause", linkage="header-only", notice="LICENSE"),
    dict(key="CGAL", name="CGAL", license="GPL-3.0-or-later", linkage="header-only", link_token="libslic3r_cgal",
         notice="LICENSE.md", note="Header-only GPL-3.0+ packages are compiled into libslic3r_cgal; GPL-3.0 is combinable with AGPL-3.0."),
    dict(key="OpenVDB", name="OpenVDB (Prusa fork)", license="MPL-2.0", linkage="static", link_token="libopenvdb.a", notice="LICENSE"),
    dict(key="Blosc", name="c-blosc", license="BSD-3-Clause", linkage="static", link_token="libblosc.a", notice="LICENSES/BLOSC.txt",
         note="Also bundles LZ4, Snappy, FastLZ, Zlib parts, see LICENSES/ in the source tree."),
    dict(key="OpenEXR", name="OpenEXR / Half / Iex / Imath", license="BSD-3-Clause", linkage="static", link_token="libHalf-2_5.a", notice="LICENSE.md"),
    dict(key="GMP", name="GMP", license="LGPL-3.0-or-later OR GPL-2.0-or-later", linkage="static", link_token="libgmp.a", notice="COPYING.LESSERv3",
         note="Statically linked LGPL library: the corresponding source and build recipe (deps/) must stay available for relinking."),
    dict(key="MPFR", name="MPFR", license="LGPL-3.0-or-later", linkage="static", link_token="libmpfr.a", notice="COPYING.LESSER",
         note="Statically linked LGPL library, same relinking note as GMP."),
    dict(key="NLopt", name="NLopt", license="MIT AND LGPL-2.1-or-later", linkage="static", link_token="libnlopt.a", notice="COPYRIGHT",
         note="Combined work is under all included terms; most restrictive: luksan/ is LGPL-2.1+ (luksan/COPYRIGHT)."),
    dict(key="Qhull", name="Qhull", license="Qhull", linkage="static", link_token="libqhullcpp.a", notice="COPYING.txt",
         note="Qhull license is a permissive BSD-style license (not an SPDX BSD id)."),
    dict(key="wxWidgets", name="wxWidgets (Prusa fork)", license="LicenseRef-wxWindows-3.1", linkage="static", link_token="libwx_gtk3u_core",
         notice="docs/licence.txt", note="wxWindows Library Licence 3.1 = LGPL-2+ with exception allowing binary distribution under own terms."),
    dict(key="GLEW", name="GLEW", license="BSD-3-Clause AND MIT", linkage="static", link_token="libGLEW.a", notice="LICENSE.txt"),
    dict(key="NanoSVG", name="NanoSVG", license="Zlib", linkage="static", link_token="libnanosvg.a", notice="LICENSE.txt"),
    dict(key="EXPAT", name="Expat", license="MIT", linkage="static", link_token="libexpat.a", notice="expat/COPYING"),
    dict(key="ZLIB", name="zlib", license="Zlib", linkage="static", link_token="libz.a", notice="LICENSE",
         note="libz.a from deps/ is linked statically; the binary additionally needs the system libz.so.1 through OpenCV/libpng."),
    dict(key="z3", name="Z3", license="MIT", linkage="static", link_token="libz3.a", notice="LICENSE.txt"),
    dict(key="OCCT", name="Open CASCADE Technology", license="LGPL-2.1-only WITH OCCT-exception-1.0", linkage="plugin",
         link_token=None, notice="LICENSE_LGPL_21.txt",
         note="Built as OCCTWrapper.so (STEP import), loaded at runtime; not linked into the main executable."),
    dict(key="json", name="nlohmann/json", license="MIT", linkage="header-only", notice="LICENSE.MIT"),
    # Built by deps/ but excluded from the offline SLA binary (kept in the SBOM so nobody assumes otherwise).
    dict(key="CURL", name="curl", license="curl", linkage="not-linked", link_token="libcurl.a", notice="COPYING",
         note="Excluded from the deps/ build, not linked (offline-only application)."),
    dict(key="OpenSSL", name="OpenSSL", license="OpenSSL AND SSLeay", linkage="not-linked", link_token="libssl.a", notice="LICENSE",
         note="Excluded from the deps/ build, not linked (offline-only application; verified by test sla_offline_no_network_libs)."),
    dict(key="JPEG", name="libjpeg-turbo", license="BSD-3-Clause AND IJG AND Zlib", linkage="not-linked", link_token="libjpeg.a",
         notice="LICENSE.md", note="Built by deps/ but the binary uses the system libjpeg.so.8 (see runtime libraries)."),
    dict(key="PNG", name="libpng", license="Libpng", linkage="not-linked", link_token="libpng.a", notice=None,
         note="Binary uses the system libpng16.so.16 (see runtime libraries)."),
    dict(key="Catch2", name="Catch2", license="BSL-1.0", linkage="not-linked", link_token=None, notice="LICENSE.txt",
         note="Test-only, never part of the distributed application."),
]

# bundled_deps/ and in-tree third-party code. version: regex applied to a file, or a literal.
BUNDLED = [
    dict(key="admesh", name="ADMesh", license="GPL-2.0-or-later", linkage="static", link_token="libadmesh.a",
         version=("bundled_deps/admesh/admesh/stl.h", None), notice="bundled_deps/admesh/admesh/stl.h (header)",
         note="GPL-2.0-or-later is combinable with AGPL-3.0."),
    dict(key="agg", name="Anti-Grain Geometry", license="BSD-3-Clause", linkage="header-only", link_token=None,
         version=("bundled_deps/agg/agg/VERSION", r"(\d+\.\d+)"), notice="bundled_deps/agg/agg/copying",
         note="AGG 2.4 modified-BSD option of the dual license."),
    dict(key="miniz", name="miniz (Prusa-modified)", license="MIT", linkage="static", link_token="libminiz_static.a",
         version=("bundled_deps/miniz/miniz.h", r'MZ_VERSION\s+"([^"]+)"'), notice="bundled_deps/miniz/LICENSE"),
    dict(key="imgui", name="Dear ImGui", license="MIT", linkage="static", link_token="libimgui.a",
         version=("bundled_deps/imgui/imgui/imgui.h", r'IMGUI_VERSION\s+"([^"]+)"'), notice="bundled_deps/imgui/LICENSE.txt"),
    dict(key="hidapi", name="hidapi", license="BSD-3-Clause", linkage="static", link_token="libhidapi.a",
         version=("bundled_deps/hidapi/README.md", r"hidapi-(\d+\.\d+\.\d+)"), notice="bundled_deps/hidapi/README.md",
         note="Upstream is tri-licensed (GPL-3 / BSD / original HIDAPI); the BSD option is used."),
    dict(key="libigl", name="libigl (subset)", license="MPL-2.0", linkage="header-only", link_token=None,
         version=None, notice="per-file MPL-2.0 header in bundled_deps/libigl/igl/*"),
    dict(key="libnest2d", name="libnest2d", license="LGPL-3.0-only", linkage="header-only", link_token=None,
         version=None, notice="bundled_deps/libnest2d/LICENSE.txt",
         note="Header-style LGPL-3.0 code compiled into the application; AGPL-3.0 combination allowed by LGPL section 4."),
    dict(key="glu-libtess", name="GLU libtess (Mesa)", license="SGI-B-2.0", linkage="static", link_token="libglu-libtess.a",
         version=None, notice="bundled_deps/glu-libtess/src/*.c (SGI FREE SOFTWARE LICENSE B 2.0 header)"),
    dict(key="qoi", name="QOI", license="MIT", linkage="static", link_token="libqoi.a",
         version=("bundled_deps/qoi/README.md", r"commit ([0-9a-f]{7})"), notice="bundled_deps/qoi/qoi.h (license block)"),
    dict(key="semver", name="semver.c", license="MIT", linkage="static", link_token="libsemver.a",
         version=None, notice="bundled_deps/semver/semver.c (header)"),
    dict(key="fast_float", name="fast_float", license="Apache-2.0 OR MIT", linkage="header-only", link_token=None,
         version=("bundled_deps/fast_float/README.md", r"fast_float-(\d+\.\d+\.\d+)"), notice="bundled_deps/fast_float/fast_float.h"),
    dict(key="ankerl", name="unordered_dense", license="MIT", linkage="header-only", link_token=None,
         version=("bundled_deps/ankerl/README.txt", r"unordered_dense (\d+\.\d+\.\d+)"), notice="bundled_deps/ankerl/ankerl/*.h (SPDX header)"),
    dict(key="stb_dxt", name="stb_dxt", license="MIT OR Unlicense", linkage="header-only", link_token=None,
         version=None, notice="bundled_deps/stb_dxt/stb_dxt.h (end of file)"),
    dict(key="stb_image", name="stb_image_resize2", license="MIT OR Unlicense", linkage="header-only", link_token=None,
         version=("bundled_deps/stb_image/stb_image_resize2.h", r"stb_image_resize2 - v(\d+\.\d+)"), notice="bundled_deps/stb_image/stb_image_resize2.h (end of file)"),
    dict(key="tcbspan", name="tcb::span", license="BSL-1.0", linkage="header-only", link_token=None,
         version=("bundled_deps/tcbspan/README.md", r"commit ([0-9a-f]{7})"), notice="bundled_deps/tcbspan/README.md"),
    dict(key="int128", name="Clipper Int128 excerpt", license="BSL-1.0", linkage="header-only", link_token=None,
         version=None, notice="bundled_deps/int128/Int128.hpp (Clipper license block)"),
    dict(key="clipper", name="Clipper (Angus Johnson)", license="BSL-1.0", linkage="static", link_token="libclipper.a",
         version=("src/clipper/clipper.hpp", r"Version\s*:\s*(\d+\.\d+\.\d+)"), notice="src/clipper/clipper.hpp (license block)",
         note="In-tree copy with PrusaSlicer changes (also src/libslic3r/clipper.*)."),
]

# Fonts / data shipped in resources/ (not code, but distributed).
RESOURCES = [
    dict(key="NotoSans", name="Noto Sans Regular", license="OFL-1.1", path="resources/fonts/NotoSans-Regular.ttf",
         notice="resources/fonts/README.txt"),
    dict(key="NotoSansCJK", name="Noto Sans CJK Regular", license="OFL-1.1", path="resources/fonts/NotoSansCJK-Regular.ttc",
         notice="resources/fonts/README.txt"),
    dict(key="hershey", name="Hershey vector fonts (via OpenCV 4.12 table)", license="Apache-2.0",
         path="src/libslic3r/SLA/HersheyFontData.hpp", notice="doc/licenses/OpenCV-Apache-2.0.txt, doc/licenses/Hershey-import.json"),
    dict(key="icons", name="Application icons", license="AGPL-3.0-only", path="resources/icons",
         notice="LICENSE (PrusaSlicer resources; no separate third-party icon notices in the tree)"),
    dict(key="profiles", name="Vendor printer/material profiles", license="AGPL-3.0-only", path="resources/profiles",
         notice="LICENSE; vendor and product names/trademarks belong to their owners"),
    dict(key="shapes", name="Gallery shapes (3DBenchy etc.)", license="LicenseRef-see-THIRD_PARTY_LICENSES", path="resources/shapes",
         notice="doc/THIRD_PARTY_LICENSES.md section on resources (3DBenchy is CC BY-ND 4.0 upstream; no notice in the tree)"),
    dict(key="wx_locale", name="wxWidgets message catalogs", license="LicenseRef-wxWindows-3.1", path="resources/localization/wx_locale",
         notice="deps wxWidgets docs/licence.txt"),
]


def read(path, limit=None):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read(limit) if limit else handle.read()
    except OSError:
        return ""


def deps_version(repo, key):
    """Return (version, url) from deps/+<key>/*.cmake, ignoring commented lines."""
    folder = repo / "deps" / ("+" + key)
    urls = []
    for cmake in sorted(folder.glob("*.cmake")):
        for line in read(cmake).splitlines():
            stripped = line.strip()
            if stripped.startswith("#") or "URL_HASH" in stripped:
                continue
            match = re.search(r"URL\s+\"?(https?://[^\s\")]+)", stripped)
            if match:
                urls.append(match.group(1))
    if not urls:
        return None, None
    url = urls[0]
    commit = re.search(r"/([0-9a-f]{40})\.(?:zip|tar)", url)
    if commit:
        return "git-" + commit.group(1)[:12], url
    zlib = re.search(r"/v(\d+\.\d+\.\d+)/zlib\d+\.zip", url)
    if zlib:
        return zlib.group(1), url
    ver = re.search(r"[/-](?:R_|[vV])?(\d+(?:[._]\d+)+(?:-alpha\d+)?[a-z]?)(?:-release)?(?:\.zip|\.tar|/|$)", url)
    if not ver:
        ver = re.search(r"(?:OpenSSL|opencsg)[_-](\d+(?:[_-]\d+)+[a-z]?)", url)
    if not ver:
        return None, url
    return ver.group(1).replace("_", ".").replace("-", ".") if not "alpha" in ver.group(1) else ver.group(1), url


def bundled_version(repo, spec):
    if not spec:
        return None
    path, pattern = spec
    if pattern is None:
        return None
    match = re.search(pattern, read(repo / path))
    return match.group(1) if match else None


def link_tokens(binary_dir):
    """Tokens of the final link line of the main executable plus the GUI/libslic3r archives."""
    text = ""
    for rel in ("src/CMakeFiles/PrusaSlicer.dir/link.txt",):
        text += read(binary_dir / rel)
    return text


def deps_prefix_exists(repo, key):
    return (repo / "deps" / "build" / f"dep_{key}-prefix" / "src" / f"dep_{key}").is_dir()


def notice_path(repo, key, rel):
    if not rel:
        return None
    base = f"deps/build/dep_{key}-prefix/src/dep_{key}/{rel}"
    return base if (repo / base).exists() else f"upstream source archive: {rel}"


def uvtools_ports(repo):
    """Files in src/ that mention UVtools: either a native port or a UVtools-compatible codec."""
    result = subprocess.run(["grep", "-rliE", "uvtools", "src"], cwd=repo, capture_output=True, text=True)
    return sorted(line for line in result.stdout.splitlines() if line)


def runtime_libs(binary):
    """soname -> resolved path from ldd; empty when the binary is missing."""
    if not binary.exists() or not shutil.which("ldd"):
        return {}
    out = subprocess.run(["ldd", str(binary)], capture_output=True, text=True).stdout
    libs = {}
    for line in out.splitlines():
        match = re.match(r"\s*(\S+)\s+=>\s+(\S+)", line)
        if match:
            libs[match.group(1)] = match.group(2)
        elif line.strip().startswith("/"):
            path = line.split()[0]
            libs[os.path.basename(path)] = path
    return libs


def package_owner(path):
    """Best effort distro package owning a file (pacman, dpkg, rpm); None when unknown."""
    for cmd in (["pacman", "-Qo", "-q", path], ["dpkg", "-S", path], ["rpm", "-qf", path]):
        if shutil.which(cmd[0]):
            res = subprocess.run(cmd, capture_output=True, text=True)
            if res.returncode == 0 and res.stdout.strip():
                return res.stdout.strip().splitlines()[0].split(":")[0]
    return None


def lib_version(path):
    real = os.path.realpath(path)
    match = re.search(r"\.so\.(\d[\d.]*)", os.path.basename(real))
    return match.group(1).rstrip(".") if match else None


def component(name, version, license_expr, scope="required", **extra):
    entry = {"type": "library", "bom-ref": f"{name}@{version or 'unknown'}".replace(" ", "_"), "name": name}
    if version:
        entry["version"] = version
    if license_expr:
        entry["licenses"] = [{"expression": license_expr}]
    entry["scope"] = scope
    props = extra.pop("properties", {})
    entry["properties"] = [{"name": f"slasl:{k}", "value": str(v)} for k, v in props.items() if v is not None]
    entry.update(extra)
    return entry


def build(repo, binary_dir, binary):
    link = link_tokens(binary_dir)
    have_link = bool(link)
    components = []

    def linked(token):
        if token is None:
            return "n/a"
        return ("yes" if token in link else "no") if have_link else "unknown"

    for dep in DEPS:
        version, url = deps_version(repo, dep["key"])
        status = linked(dep.get("link_token"))
        # Dependencies built but not linked are excluded from the distributed application.
        scope = "excluded" if dep["linkage"] == "not-linked" or status == "no" else "required"
        extra = dict(properties={
            "source": "deps/+" + dep["key"], "linkage": dep["linkage"], "linked-in-binary": status,
            "notice": notice_path(repo, dep["key"], dep.get("notice")), "note": dep.get("note"),
            "source-built": deps_prefix_exists(repo, dep["key"])})
        if url:
            extra["externalReferences"] = [{"type": "distribution", "url": url}]
        components.append(component(dep["name"], version, dep["license"], scope, **extra))

    for item in BUNDLED:
        status = linked(item.get("link_token"))
        extra = dict(properties={
            "source": "bundled_deps/" + item["key"] if item["key"] != "clipper" else "src/clipper",
            "linkage": item["linkage"], "linked-in-binary": status, "notice": item.get("notice"), "note": item.get("note")})
        components.append(component(item["name"], bundled_version(repo, item.get("version")), item["license"],
                                    "required", **extra))

    for item in RESOURCES:
        present = (repo / item["path"]).exists()
        components.append(component(item["name"], None, item["license"], "required" if present else "excluded",
                                    type="data", properties={"path": item["path"], "notice": item["notice"],
                                                             "linkage": "data"}))

    libs = runtime_libs(binary)
    for soname, path in sorted(libs.items()):
        if soname.startswith("linux-vdso") or "ld-linux" in soname:
            continue
        components.append(component(soname, lib_version(path), None, "required", properties={
            "linkage": "dynamic (system)", "resolved-path": path, "package": package_owner(path),
            "note": "license = that of the distribution package; not bundled unless tools/make_relocatable_bundle.sh is used"}))

    ports = uvtools_ports(repo)
    components.append(component("UVtools (algorithms ported natively)", None, "AGPL-3.0-only", "required", properties={
        "linkage": "source port, no UVtools code/binary at runtime",
        "attribution": "Tiago Conceicao (sn4k3) and UVtools contributors, https://github.com/sn4k3/UVtools",
        "files-mentioning-uvtools": len(ports), "files": ", ".join(ports)}))
    return components, ports


def main():
    here = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=here)
    parser.add_argument("--build", type=Path, default=None, help="build dir (default: <repo>/build)")
    parser.add_argument("--binary", type=Path, default=None, help="sla-slicer binary for ldd")
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()
    repo = args.repo.resolve()
    build_dir = (args.build or repo / "build").resolve()
    binary = (args.binary or build_dir / "src" / "sla-slicer").resolve()
    output = args.output or repo / "doc" / "SBOM.json"

    components, ports = build(repo, build_dir, binary)
    stamp = datetime.datetime.fromtimestamp(int(os.environ.get("SOURCE_DATE_EPOCH", datetime.datetime.now().timestamp())),
                                            datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    version_inc = read(repo / "version.inc")
    app_version = (re.search(r'SLA_SLICER_VERSION\s+"([^"]+)"', version_inc) or [None, "1.0.0"])[1]
    document = {
        "bomFormat": "CycloneDX", "specVersion": "1.5", "version": 1,
        # Deterministic serial: same inventory -> same URN, so diffs show real changes only.
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, json.dumps(
            [(c["name"], c.get("version")) for c in components]))),
        "metadata": {
            "timestamp": stamp,
            "tools": {"components": [{"type": "application", "name": "tools/generate_sbom.py"}]},
            "component": {"type": "application", "name": APP_NAME, "version": app_version,
                          "licenses": [{"expression": APP_LICENSE}],
                          "description": "SLA/MSLA slicer derived from PrusaSlicer 2.9.4 (AGPL-3.0)"},
        },
        "components": components,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    kinds = {}
    for comp in components:
        kinds[comp["scope"]] = kinds.get(comp["scope"], 0) + 1
    print(f"wrote {output}: {len(components)} components {kinds}, {len(ports)} files mention UVtools")
    if not binary.exists():
        print(f"warning: {binary} missing, runtime libraries not listed", file=sys.stderr)


if __name__ == "__main__":
    main()
