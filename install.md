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
  libwebkit2gtk-4.1-dev \
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

The launcher auto-detects a valid CA bundle and exports `SSL_CERT_FILE` when needed.
This avoids startup/runtime TLS issues on distros that do not use the default OpenSSL cert path.

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
