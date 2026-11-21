# SLA-Slicer

SLA-Slicer 1.0.0 is a resin (SLA/MSLA, LCD-mask printer) slicer based on the
PrusaSlicer 2.9.4 code base, limited to SLA printing and working fully offline.
There is no G-code, no FFF/FDM printing and no network features (upload to printer,
updates, accounts).

SLA-Slicer is not affiliated with Prusa Research. PrusaSlicer is a trademark/product
of Prusa Research; the name is used here only to state where the code comes from.

## Key features

- Slicing 3D models (STL, OBJ, 3MF) into mask layers with supports, hollowing, pad
  and a layer preview.
- Export to 80 printer format identifiers (SL1/SL1S, Anycubic, CTB/CBDDLP/Photon,
  GOO, CXDLP, Klipper, NanoDLP, ZCode and more; full list in
  [doc/COMPATIBILITY_INVENTORY.md](doc/COMPATIBILITY_INVENTORY.md)).
- Layer editor (Sliced view): brush, eraser, layer ranges, mask operations, issue
  detection (islands, overhangs, resin traps, suction cups), Undo/Redo, saving back
  to the printer file.
- PCB exposure from Gerber and Excellon files (solder mask, copper, drill holes).
- Calibrations matching UVtools (Exposure Finder, exposure tables, test patterns),
  computed natively in C++.
- Batch mode and automation: CLI (`sla-slicer --help-sla`), JSON results, exit
  codes — [doc/AUTOMATION.md](doc/AUTOMATION.md).
- User interface in English and Polish.

Release status and known limitations: [CHANGELOG.md](CHANGELOG.md).

## Installation

Prebuilt packages for Linux x86_64 (glibc 2.35 or newer, GTK3, OpenGL):

- AppImage: `chmod +x SLA-Slicer-1.0.0-x86_64.AppImage && ./SLA-Slicer-1.0.0-x86_64.AppImage`
  (without FUSE: add `--appimage-extract-and-run`)
- Debian/Ubuntu: `sudo apt install ./sla-slicer_1.0.0_amd64.deb`
- Fedora: `sudo dnf install ./sla-slicer-1.0.0-1.x86_64.rpm`
- Portable archive: `tar xzf sla-slicer-1.0.0-linux-x86_64.tar.gz`, then run
  `sla-slicer-1.0.0/sla-slicer`.

Verify downloads with `sha256sum -c SHA256SUMS`. Details (requirements, native
packages, uninstalling): [install.md](install.md).

## Building from source

Linux (recommended, GTK3):

```sh
bash ./build_linux.sh --run          # checks requirements, builds dependencies and the app
bash ./run_sla_slicer.sh             # run the locally built app
bash ./build_linux.sh --help         # options: --tests, --jobs N, --bundled-opencv, --docker, ...
```

`--docker` builds the portable archive in an Ubuntu 22.04 container. Package
requirements for Debian/Ubuntu, Fedora and Arch, and notes on OpenCV:
[install.md](install.md).

Windows: `build_win.bat` — **EXPERIMENTAL, untested** (no MSVC toolchain was
available to the author; the script is an adapted version of PrusaSlicer's).

## User data

Configuration lives in `~/.config/SLA-Slicer` (file `SLA-Slicer.ini`), separate from
an installed PrusaSlicer. On first start, if the new directory does not exist but
`~/.config/PrusaSlicer` does, its SLA part (printer, material and print profiles,
shapes, resin stock) is COPIED into the new directory; the PrusaSlicer directory is
left untouched. Set `SLA_SLICER_NO_MIGRATION=1` to disable this.
To try things out in a separate directory: `sla-slicer --datadir /path`.

## Documentation

- [User manual](manual.txt) (in Polish): profiles, models, slicing, layer editor,
  formats, PCB, calibrations, CLI, troubleshooting
- [Building and installation](install.md)
- [Changelog and known limitations](CHANGELOG.md)
- [Developer documentation](doc/DEVELOPER.md), [testing](doc/TESTING.md),
  [automation](doc/AUTOMATION.md), [layer editor](doc/LAYER_EDITOR.md),
  [PCB](doc/PCB_CALIBRATION.md), [calibration architecture](doc/EXPOSURE_CALIBRATION_ARCHITECTURE.md),
  [translations](doc/Localization_guide.md), [dependencies](doc/Dependencies.md)
- [Component licenses](doc/THIRD_PARTY_LICENSES.md) and [SBOM](doc/SBOM.json) (CycloneDX 1.5)

## License

AGPL-3.0-only (see [LICENSE](LICENSE)). PrusaSlicer, from which the code is derived,
is AGPLv3 or later; UVtools is AGPLv3 only, so the combined work is AGPL-3.0-only
(see [doc/THIRD_PARTY_LICENSES.md](doc/THIRD_PARTY_LICENSES.md) §1).
UVtools algorithms ported natively to C++ (issue detection, layer operations,
exports, calibrations, codecs) remain under AGPL-3.0 with credit to the author:
Tiago Conceição (sn4k3) and UVtools contributors, <https://github.com/sn4k3/UVtools>;
the application contains no UVtools code or process. Third-party libraries (Boost,
wxWidgets, OpenVDB, CGAL, GMP/MPFR, OpenCV and others) carry their own licenses:
[doc/THIRD_PARTY_LICENSES.md](doc/THIRD_PARTY_LICENSES.md).
