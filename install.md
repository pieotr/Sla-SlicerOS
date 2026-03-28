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

These changes are build-system-only. No slicer runtime logic was modified.

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
```

- `--clean`: remove previous build directories first.
- `--jobs N`: app build parallelism.
- `--run`: run smoke test after build.

## Run the app

```bash
bash ./run_prusa_slicer.sh
```

The launcher auto-detects a valid CA bundle and exports `SSL_CERT_FILE` when needed.
This avoids startup/runtime TLS issues on distros that do not use the default OpenSSL cert path.
