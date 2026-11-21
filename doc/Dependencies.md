# Zależności SLA-Slicer

Aktualny graf celów: `CMakeLists.txt`, `src/CMakeLists.txt`, `src/libslic3r/CMakeLists.txt`
i `deps/CMakeLists.txt`. Licencje składników: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md),
spis maszynowy: [SBOM.json](SBOM.json). Aplikacja jest tylko offline: CURL i OpenSSL są wyłączone z
budowy zależności i nie są linkowane; biblioteki WebView nie są używane.

## Budowane przez `deps/` (statycznie, katalog `deps/build/destdir/usr/local`)

Blosc, Boost, Catch2, Cereal, CGAL, Eigen, EXPAT, GLEW, GMP, JPEG, MPFR, NanoSVG, NLopt, OCCT (STEP),
OpenEXR, OpenVDB, PNG (Windows/macOS), Qhull, TBB, ZLIB (Windows), json (nlohmann), wxWidgets
(GTK3 na Linuksie; opcja `-DDEP_WX_GTK3=ON`), z3.

**OpenCV 4.12.0** (core, imgproc; statycznie): receptura `deps/+OpenCV/OpenCV.cmake`. Budowana
domyślnie na Windows i macOS; na Linuksie tylko na żądanie (`-DDEP_OpenCV=ON`,
`build_linux.sh --bundled-opencv`) — domyślnie używany jest OpenCV z systemu (>= 4.7). Ta sama wersja
i suma SHA256 co w `tools/docker/Dockerfile.build`.

## Z systemu (Linux)

GTK3, OpenGL/GLU, dbus, X11, libtiff, libpng, libjpeg, OpenJPEG, zlib, OpenCV >= 4.7 (jeśli nie
użyto `--bundled-opencv`). Pełna lista z nazwami pakietów dla Debiana/Ubuntu, Fedory i Archa:
[../install.md](../install.md).

## W drzewie źródeł (`bundled_deps/`, `src/`)

admesh, agg, ankerl, fast_float, glu-libtess, hidapi (mysz 3D), imgui, int128, libigl, libnest2d,
localesutils, miniz, qoi, semver, stb_dxt, stb_image, tcbspan, clipper (z modyfikacjami),
slic3r-arrange, occt_wrapper. Dane wskazówek: `bundled_deps/hints`.

Dla pakietów przenośnych biblioteki natywne, których nie dostarcza system (OpenCV z kontenera, TIFF,
OpenJPEG, libjpeg, ...), zbiera `tools/bundle_native_libraries.py` wraz z licencjami i sumami SHA256.
