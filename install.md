# Linux Build and Run (one command)

This repository now includes a one-command build script:

- `build_linux.sh`

It builds dependencies and the app in the documented static mode, then can run a smoke test.

## What was changed

To make modern Linux toolchains build reliably, the dependency build scripts were hardened:

1. `deps/CMakeLists.txt`
- `DEP_CMAKE_OPTS` now also forwards `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- This avoids third-party configure failures under newer CMake versions where old `cmake_minimum_required` compatibility is removed.

2. `deps/+GMP/GMP.cmake`
- Explicitly passes selected compilers to configure (`CC`, `CXX`).
- Splits C and C++ flags instead of sharing C++ flags for C checks.
- Adds `-std=gnu17` for GCC 15+ C compiler checks, which fixes GMP configure failures on modern GCC.

3. `deps/+MPFR/MPFR.cmake`
- Removes reliance on leaked variables from other dependency files.
- Uses explicit, local C/C++ flags and host/build target handling.
- Passes explicit compilers (`CC`, `CXX`) so MPFR matches the GMP toolchain.

These dependency fixes are separate from the ongoing application refactor.
See [implementation status](doc/REFACTOR_STATUS.md) for runtime changes.

## Prerequisites

Install required system packages (example from project docs):

```bash
sudo apt-get install -y \
  git \
  build-essential \
  autoconf \
  cmake \
  libglu1-mesa-dev \
  libgtk-3-dev \
  libdbus-1-dev \
  texinfo
```

## One-command build

From repo root:

```bash
bash ./build_linux.sh --run
```

This will:

1. Configure and build static deps in `deps/build`.
2. Configure and build the app in `build`.
3. Run a smoke test (`prusa-slicer --help`) if `--run` is provided.

Current defaults in `build_linux.sh` are:

- `-DSLIC3R_SLA_ONLY=ON`
- `-DSLIC3R_OFFLINE_ONLY=ON`
- `-DSLIC3R_BUILD_TESTS=OFF`

This keeps the build focused on SLA workflows and improves incremental iteration speed.

## Useful options

```bash
bash ./build_linux.sh --clean --run
bash ./build_linux.sh --jobs 4
bash ./build_linux.sh --jobs 4 --tests --run
```

- `--clean`: remove previous build directories first.
- `--jobs N`: app build parallelism.
- `--run`: run smoke test after build.
- `--tests`: enable the SLA test target and run fast archive regression and SL1
  slicing/readback checks. This does not certify all formats. See
  [testing instructions](doc/TESTING.md) for suite selection and known missing
  fixtures in the legacy test suites.

`--run` alone checks `--help`; it does not open or exercise the GUI.

## Run the app

```bash
bash ./run_prusa_slicer.sh
```

The launcher (`run_prusa_slicer.sh`) passes all arguments to `build/src/prusa-slicer`
(`sla-slicer` is a symlink to it). Useful ways to start:

```bash
bash ./run_prusa_slicer.sh                              # normal start, your usual profiles
bash ./run_prusa_slicer.sh --datadir /path/to/trial     # isolated profiles for experiments
bash ./run_prusa_slicer.sh --help-sla                   # full list of SLA command-line options
bash ./run_mid_refactor_tests.sh                        # persistent separate profiles in build/mid-refactor-user-tests
```

Usage, profiles and the command line are described in [manual.txt](manual.txt)
(Polish); tests in [doc/TESTING.md](doc/TESTING.md); code layout in
[doc/DEVELOPER.md](doc/DEVELOPER.md).

The launcher starts the offline SLA application without configuring TLS certificates.
Application transport code and printer upload backends are excluded; CURL and
OpenSSL are no longer direct application dependencies. The dependency configuration
also excludes them. Initial dependency builds can still download source archives
if those archives are missing locally; prepare the dependency cache in advance
for a fully offline build. Running the application does not run that build script.

In the offline build (`SLIC3R_OFFLINE_ONLY=ON`, the default here) wxWidgets'
`webview` component is not requested and no WebView source is compiled, so
`libwebkit2gtk`, `libjavascriptcoregtk` and `libsoup` are no longer linked.
Check an existing binary with:

```bash
python3 tests/architecture/verify_offline_no_network_libs.py --binary build/src/prusa-slicer
```

(After pulling this change, re-run `cmake` on `build/` and rebuild; the check
fails on binaries built before it. The bundled wxWidgets static libraries in
`deps/` are unchanged, so no dependency rebuild is needed. Untested.)

## Product name, binary name and user data

- User-visible name: **SLA-Slicer** (`SLIC3R_APP_NAME` in `version.inc`).
- The storage key `SLIC3R_APP_KEY` intentionally remains `PrusaSlicer`. It names the
  data directory (`~/.config/PrusaSlicer`, or `$XDG_CONFIG_HOME/PrusaSlicer`) and
  `PrusaSlicer.ini`, so existing presets, profiles and settings keep working with no
  migration step and nothing is copied or moved. Renaming the key would orphan them
  (the datadir lookup lives in `libslic3r/Utils/DirectoriesUtils.cpp`, which a
  branding change does not touch). Use `--datadir <dir>` to run with an isolated directory.
- The executable stays `prusa-slicer`; `sla-slicer` is provided as a symlink next to it
  (build tree and install). Tests and scripts that reference `build/src/prusa-slicer`
  are unaffected.

## Install and relocatable bundle

Plain install (non-FHS layout, default; installs `bin/prusa-slicer`, `bin/sla-slicer`
and `resources/`; no G-code viewer files, no udev rules in SLA-only builds):

```bash
cmake --install build --prefix /opt/sla-slicer      # untested
/opt/sla-slicer/bin/sla-slicer
```

The executable finds its resources at `<exe dir>/../resources` (`src/CLI/Setup.cpp`),
so the install tree can be moved anywhere. A `-DSLIC3R_FHS=1` configure instead
bakes the absolute `<prefix>/share/SLA-Slicer` path and is not relocatable.

Relocatable tarball from the local build (needs a finished `bash ./build_linux.sh`):

```bash
bash tools/make_relocatable_bundle.sh            # writes dist/sla-slicer-<version>-linux-x86_64.tar.gz
tar xzf dist/sla-slicer-*-linux-x86_64.tar.gz -C /somewhere && /somewhere/sla-slicer-*/sla-slicer
```

It uses `tools/bundle_native_libraries.py`, then removes every library the host must
provide: glibc, libstdc++, the GTK/GL/X11/Wayland stack **and the libraries that stack
itself uses** (libmount, libblkid, libselinux, libpcre, libffi, systemd, compression,
libpng, expat, ...). Only application-specific libraries stay bundled (OpenCV, TBB,
libtiff + jbig/deflate, OpenJPEG, libjpeg, libwebp). Such a bundle runs only on
distributions whose glibc is at least as new as the build machine's (a CachyOS build
needs glibc 2.43 — Arch/Fedora only).

### Portable Linux build (Docker, glibc 2.35)

For a bundle that runs on older distributions, build inside an Ubuntu 22.04 container
(glibc 2.35, GCC 12). Needs a running docker daemon and network access during the
build (apt, PyPI, dependency sources); the result itself is offline.

```bash
JOBS=8 bash tools/docker/build_portable.sh            # default work dir: ./build-docker (gitignored)
# -> build-docker/dist/sla-slicer-<version>/ and sla-slicer-<version>-linux-x86_64.tar.gz
tests/dist/bundle_in_docker.sh build-docker/dist/sla-slicer-<version> \
    ubuntu:22.04 ubuntu:24.04 debian:12 debian:testing fedora:latest archlinux:latest
```

`tools/docker/Dockerfile.build` defines the image, `tools/docker/container_build.sh` builds
the dependency superbuild (first run: hours; cached in `build-docker/work`, ccache in
`build-docker/ccache`), the app (`SLIC3R_SLA_ONLY`, `SLIC3R_OFFLINE_ONLY`), runs the
CTest subset that needs no display, and packs the bundle. `BUNDLE_LIBSTDCXX=1` also
bundles libstdc++/libgcc_s (not needed for the targets below).

Verified 14 November 2026. CLI: `tests/dist/bundle_in_docker.sh` (`ldd`, `--help`, slicing a cube to
GOO and SL1 with a fresh data directory; container CTest 22/22). GUI: `tests/dist/bundle_gui_in_docker.sh`
(Xvfb + Mesa software OpenGL, fresh data directory: the main window and the first-run Configuration
Wizard appear, no error dialog, process alive):

| Distribution | glibc | CLI (slice GOO + SL1) | GUI start (Xvfb) |
|---|---|---|---|
| Ubuntu 22.04 | 2.35 | PASS  PASS |
| Ubuntu 24.04 | 2.39 | PASS  PASS |
| Debian 12 | 2.36 | PASS  PASS |
| Debian testing | 2.43 | PASS  PASS |
| Fedora (latest) | 2.44 | PASS  PASS |
| Arch Linux (latest) | 2.44 | PASS  PASS |

Bundle requirements: glibc ≥ 2.35, libstdc++ with `GLIBCXX_3.4.30` (GCC ≥ 12), and on
the host GTK3, OpenGL (Mesa or vendor driver), dbus, libSM/libICE and libnotify — all
present on a normal desktop, plus an installed UTF-8 locale (without any, the app reports
"Switching language failed" — minimal container images only). Not verified: a real GPU driver
on another distribution (the container uses Mesa software rendering).

## Existing builds after a compiler update

Static libraries containing GCC LTO bytecode must match the compiler used for
linking. A message such as `libtbb.a ... generated with LTO version ... instead
of the expected ...` means the cached dependency needs rebuilding; disabling
application LTO does not repair the library.

For the existing bundled TBB source/build layout, rebuild only that dependency:

```sh
cmake -S deps/build/dep_TBB-prefix/src/dep_TBB -B deps/build/builds/TBB \
  -DTBB_STRICT=OFF
cmake --build deps/build/builds/TBB --clean-first -j 4
cmake --install deps/build/builds/TBB
```

These commands require a previously configured local dependency build, including
its install prefix inside `deps/build/destdir/usr/local`. `TBB_STRICT=OFF` allows
new compiler warnings in the older bundled TBB source without treating them as
errors. Then rebuild the application and rerun the tests. Other compiler/ABI
changes may require rebuilding all dependencies using `--clean --deps`.


## Native ExposureFinder drawing dependency

Building the app now also requires local OpenCV core/imgproc development files.
The verified development version is OpenCV 5.0.0 on Linux. Set `OpenCV_DIR` if
its CMake configuration is outside the standard search path. Configure/build
must use locally prepared dependencies for an offline build. No UVtools/Emgu
or .NET runtime is used by production calibration code.

For Linux packaging, `tools/bundle_native_libraries.py` copies the app ELF's
resolved native-library closure into a new project-local folder and records
SHA256 provenance. It retains OpenCV's supplied notices. This component does
not package resources/launcher or certify relocation, CPU/ABI compatibility,
or the complete license inventory of every other native dependency. Those
remain explicit release gates. Do not treat the library folder alone as a
complete portable application.
