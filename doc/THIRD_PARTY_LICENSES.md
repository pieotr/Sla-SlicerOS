# Licencje komponentów zewnętrznych

Dystrybucja. Maszynowo odczytywany odpowiednik: [SBOM.json](SBOM.json)
(CycloneDX 1.5), generowany przez `python3 -I tools/generate_sbom.py`
z definicji `deps/+*/*.cmake`, `bundled_deps/`, `build/src/CMakeFiles/PrusaSlicer.dir/link.txt`
i `ldd build/src/sla-slicer`. Po zmianie zależności uruchom skrypt ponownie i
porównaj z tym plikiem. To jest inwentarz techniczny, **nie porada prawna**;
tekst każdej licencji leży w drzewie źródeł wskazanym w kolumnie „Notka”.

Stan z 12 listopada 2026, build `build/` (Release, `-DSLIC3R_STATIC=1`). Wersje pochodzą z `deps/+*/*.cmake` (adres archiwum i skrót
SHA256 przypięte w CMake), wiązanie z `link.txt` binarium.

Uwaga (W11): heatshrink, libbgcode, OpenCSG i avrdude zostały usunięte z drzewa (`deps/`, `bundled_deps/`); wpisy usunięto z tabel.

## 1. Licencja całości

- **SLA-Slicer: AGPL-3.0** (plik `LICENSE`). Baza to PrusaSlicer 2.9.4
  (Prusa Research a.s. i współautorzy, AGPL-3.0; nagłówki źródeł mówią „AGPLv3 or higher”).
- **Algorytmy UVtools przeniesione natywnie: AGPL-3.0.** Autor: Tiago Conceição
  (sn4k3) i współautorzy UVtools, <https://github.com/sn4k3/UVtools>. Kod C# UVtools
  był wzorcem; w aplikacji nie ma kodu, DLL ani procesu UVtools (zasada projektu).
  Zachowaj to uznanie autorstwa i tekst AGPL-3.0 przy każdej dystrybucji, również
  binarnej; użytkownik sieciowy ma prawo do źródeł (AGPL §13 — aplikacja jest
  offline, ale warunek obowiązuje przy ewentualnym udostępnianiu przez sieć).
  Efektywna licencja połączonego dzieła: AGPL-3.0-only (SBOM).
- Biblioteki GPL-2.0+/GPL-3.0+/LGPL/MPL/permisywne poniżej są kompatybilne z
  AGPL-3.0; dla bibliotek LGPL linkowanych statycznie (GMP, MPFR, NLopt/luksan,
  libnest2d, wxWidgets, OCCT) trzeba zachować możliwość ponownego zlinkowania:
  pełne źródła aplikacji i przepis budowania `deps/` są dostarczane razem z kodem.

### Pliki z portami UVtools w `src/`

`grep -rliE uvtools src` zwraca ok. 85-90 plików (liczba rośnie; aktualna lista w `SBOM.json`, składnik
„UVtools (algorithms ported natively)”). Wystąpienie nazwy oznacza port algorytmu
albo kodek zgodny z formatem opisanym przez UVtools; poniżej grupy:

| Grupa | Pliki |
|---|---|
| Wykrywanie problemów (`IssueManager`) | `libslic3r/SLA/LayerIssues.hpp/.cpp`, `PixelContourTree.hpp` |
| Operacje na warstwach | `libslic3r/SLA/LayerOperations.hpp/.cpp`, `MaskFilter.hpp`, `MedianFilter.hpp`, `LayerDocument.hpp/.cpp` |
| Eksporty warstw (PNG/GIF/heatmapa/szkielet/HTML/STL) | `libslic3r/SLA/LayerExports.hpp/.cpp`, `LayerImageFile.cpp` |
| Sugestie | `libslic3r/SLA/LayerSuggestions.hpp/.cpp` |
| Kalibracje i PCB | `libslic3r/SLA/CalibrationRaster.hpp/.cpp`, `ExposureFinderPattern.cpp`, `PCBExposure.hpp` |
| Parametry/układy formatów Chitubox, CXDLP | `libslic3r/SLA/Chitubox*.hpp`, `CrealityCXDLPv4LayerParameters.hpp`, `CBDDLPFreshExportPlan.hpp`, `CWSCommandCodec.hpp` |
| Kodeki i układy plików | `libslic3r/Format/*` (AnycubicSLA, AnycubicZip, Chitubox, CTBEncryptedCrypto, CrealityCXDLP*, UVToolsCompatArchive (GOO/PRZ/UVJ/...), NanoDLP*, Klipper*, Makerbase*, Phrozen*, FDG, ZCode*, OSF, OSLA, VDT, JXS/Voxeldance, LGS/LongerOrange, Uniformation, Vlare, Emake3D, Anet, SVGX, SL1, Flashforge, PhotonS, rejestr formatów) |
| Konfiguracja, CLI, GUI | `libslic3r/PrintConfig.cpp`, `SLAPrint.cpp`, `CLI/CLI.hpp`, `CLI/ProcessActions.cpp`, `slic3r/GUI/LayerEditorDialog.cpp`, `SlaRasterToolsDialog.cpp` |

Nie każdy kodek jest portem kodu — część formatów odtworzono z publicznych
specyfikacji/wyników dekodera (patrz komentarze w plikach); mimo to uznanie
UVtools zachowujemy dla całej grupy.

## 2. Zależności statyczne i nagłówkowe (`deps/`)

Kolumna „Link”: **S** = statycznie w `sla-slicer` (potwierdzone w `link.txt`),
**H** = tylko nagłówki skompilowane w aplikację, **P** = wtyczka ładowana w
czasie działania, **—** = zbudowane w `deps/`, ale nie linkowane do binarium offline.

| Komponent | Wersja | Licencja | Link | Notka (w drzewie źródeł `deps/build/dep_<N>-prefix/src/dep_<N>/`) |
|---|---|---|---|---|
| Boost | 1.83.0 | BSL-1.0 | S | `LICENSE_1_0.txt` |
| oneTBB | 2021.5.0 | Apache-2.0 | S (`libtbb.a`, `libtbbmalloc.a`) | `LICENSE.txt` |
| Eigen | 3.3.7 | MPL-2.0 | H | `COPYING.MPL2` |
| cereal | 1.3.0 | BSD-3-Clause | H | `LICENSE` |
| CGAL | 5.6.2 | GPL-3.0-or-later | H (`liblibslic3r_cgal.a`) | `LICENSE.md` |
| OpenVDB (fork prusa3d) | commit 339ee88230da | MPL-2.0 | S | `LICENSE` |
| c-blosc | commit 8724c06e3da9 | BSD-3-Clause (+ LZ4, Snappy, FastLZ, zlib) | S | `LICENSES/*.txt` |
| OpenEXR (Half) | 2.5.5 | BSD-3-Clause | S | `LICENSE.md` |
| GMP | 6.2.1 | LGPL-3.0+ lub GPL-2.0+ | S | `COPYING.LESSERv3` |
| MPFR | 4.2.1 | LGPL-3.0+ | S | `COPYING.LESSER` |
| NLopt | 2.5.0 | MIT oraz LGPL-2.1+ (katalog `luksan`) | S | `COPYRIGHT`, `COPYING` |
| Qhull | 8.1-alpha3 | licencja Qhull (BSD-podobna) | S | `COPYING.txt` |
| wxWidgets (fork prusa3d) | commit 5462e7d7cfac | wxWindows Library Licence 3.1 | S | `docs/licence.txt` |
| GLEW | 2.2.0 | BSD-3-Clause / MIT | S | `LICENSE.txt` |
| NanoSVG | commit abcd277ea45e | zlib | S | `LICENSE.txt` |
| Expat | 2.4.3 | MIT | S | `expat/COPYING` |
| zlib | 1.3.1 | zlib | S (`libz.a`) oraz systemowe `libz.so.1` | `LICENSE` |
| Z3 | 4.15.1 | MIT | S | `LICENSE.txt` |
| nlohmann/json | 3.12.0 | MIT | H | `LICENSE.MIT` |
| Open CASCADE (OCCT) | 7.6.1 | LGPL-2.1 + wyjątek OCCT | P (`OCCTWrapper.so`, import STEP) | `LICENSE_LGPL_21.txt` |
| curl | 7.75.0 | licencja curl | — | `COPYING` |
| OpenSSL | 1.1.0l | OpenSSL + SSLeay | — | `LICENSE` |
| libjpeg-turbo | 3.0.1 | BSD-3 / IJG / zlib | — (binarium używa systemowego `libjpeg.so.8`) | `LICENSE.md` |
| libpng | 1.6.35 | libpng | — (binarium używa systemowego `libpng16.so.16`) | — |
| Catch2 | 3.8.0 | BSL-1.0 | — (tylko testy) | `LICENSE.txt` |

Uwagi: `libcurl.a`/`libcrypto.a`/`libssl.a` istnieją w `deps/build/destdir`, ale
nie występują w `link.txt`; brak NEEDED na curl/ssl sprawdza test
`sla_offline_no_network_libs`. OpenCV (rysowanie ExposureFinder) nie jest budowany
przez `deps/` — patrz sekcja 4.

## 3. Kod dołączony w drzewie (`bundled_deps/`, `src/`)

| Komponent | Wersja | Licencja | Link | Notka |
|---|---|---|---|---|
| ADMesh | (fork Prusa) | GPL-2.0-or-later | S | nagłówek `bundled_deps/admesh/admesh/stl.h` |
| Anti-Grain Geometry | 2.4 | BSD-3-Clause (wariant modified-BSD podwójnej licencji) | H | `bundled_deps/agg/agg/copying` |
| miniz (zmodyfikowany Prusa) | 10.1.0 | MIT | S | `bundled_deps/miniz/LICENSE`, `README-Prusa.txt` |
| Dear ImGui | 1.83 | MIT | S | `bundled_deps/imgui/LICENSE.txt` |
| hidapi | 0.9.0 | BSD-3-Clause (jedna z trzech opcji upstream) | S | `bundled_deps/hidapi/README.md` |
| libigl (podzbiór) | — | MPL-2.0 | H | nagłówek w każdym `igl/*.h` |
| libnest2d | — | LGPL-3.0 | H | `bundled_deps/libnest2d/LICENSE.txt` |
| GLU libtess (Mesa) | commit 0bf42e41 | SGI Free Software License B 2.0 | S | nagłówki `bundled_deps/glu-libtess/src/*.c` |
| QOI | 6c0831f | MIT | S | `bundled_deps/qoi/qoi.h` |
| semver.c | — | MIT | S | `bundled_deps/semver/semver.c` |
| fast_float | 2.0.0 | Apache-2.0 lub MIT | H | `bundled_deps/fast_float/fast_float.h` |
| unordered_dense (ankerl) | 3.1.1 | MIT | H | nagłówki `bundled_deps/ankerl/ankerl/*.h` |
| stb_dxt / stb_image_resize2 | — / 2.12 | MIT lub domena publiczna | H | koniec pliku nagłówka |
| tcb::span | 836dc6a | BSL-1.0 | H | `bundled_deps/tcbspan/README.md` |
| Clipper (A. Johnson) + Int128 | 6.4.2 | BSL-1.0 | S (`libclipper.a`) | `src/clipper/clipper.hpp`, `bundled_deps/int128/Int128.hpp` |
| localesutils, hints | — | AGPL-3.0 (kod Prusa) | S | — |

`libvgcode` (`src/libvgcode`) nie jest linkowany do `sla-slicer` (brak w `link.txt`).

## 4. Biblioteki systemowe w czasie działania (`ldd build/src/sla-slicer`)

Binarium wymaga (nie są statyczne; licencje dystrybucji, głównie LGPL/MIT/BSD/ISC):

- **GUI/graficzne:** GTK 3 (`libgtk-3`, `libgdk-3`), GLib/GObject/GIO, Pango/Cairo/HarfBuzz/
  Fontconfig/FreeType, ATK/AT-SPI, GDK-Pixbuf, libnotify, X11 (`libX11`, `libXext`, `libSM`, `libICE`),
  xkbcommon, Wayland (`client`, `egl`, `cursor`), EGL, GL/GLX/GLU/OpenGL (`libGL.so.1`, `libGLU.so.1`),
  dbus-1, epoxy.
- **Obrazy i kompresja:** OpenCV `core`+`imgproc` (paczka przenośna: 4.12.0 budowany z oficjalnego tarballa w obrazie docker; maszyna dev: 5.0.0, + pośrednio `flann`, `geometry`,
  BLAS/LAPACK, TBB `libtbb.so.12`) — **Apache-2.0**, notka w `doc/licenses/OpenCV-NOTICE.md` i
  `OpenCV-Apache-2.0.txt`; libpng16, libjpeg, libtiff, openjp2, libwebp, jbig, deflate, zstd, lzma,
  bz2, zlib (`libz.so.1`, w tej maszynie zlib-ng), libmspack, expat (`libexpat.so.1`), libxml2, ICU.
- **Środowisko:** libstdc++, libgcc_s, libm, libc (glibc), ld-linux.

Pełna lista z wersjami i pakietami: składniki z `linkage = dynamic (system)` w `SBOM.json`.
`tools/bundle_native_libraries.py` / `tools/make_relocatable_bundle.sh` kopiują część tych
bibliotek do paczki relokowalnej razem z notkami; **spoza paczki zawsze zostaje** środowisko
(glibc, libstdc++, GTK3, GL); przenośność archiwum zależy od wersji glibc hosta
(patrz `install.md`, sekcja 3). Dystrybutor paczki odpowiada za dołączenie licencji tych
bibliotek (LGPL: GTK/GLib/Cairo/Pango).

## 5. Zasoby (`resources/`, dane w kodzie)

| Zasób | Licencja | Notka |
|---|---|---|
| Noto Sans Regular, Noto Sans CJK Regular | SIL OFL-1.1 | `resources/fonts/README.txt` |
| Czcionki wektorowe Hershey (tablice w `SLA/HersheyFontData.hpp`) | Apache-2.0 (dane z tablicy OpenCV 4.12) | `doc/licenses/Hershey-import.json`, `OpenCV-Apache-2.0.txt` |
| Ikony (`resources/icons`, 319 plików), shadery, `data/`, wskazówki (`hints.ini`, `sla_hints.ini`), lokalizacje `*.po` | AGPL-3.0 (zasoby PrusaSlicer; polskie tłumaczenia wskazówek SLA: SLA-Slicer) | brak osobnych notek obcych ikon w drzewie |
| Profile drukarek/materiałów (`resources/profiles`) | AGPL-3.0 jako część repozytorium; nazwy i znaki producentów należą do producentów | do weryfikacji, czy każdy dostawca zezwala na redystrybucję danych (nie sprawdzono) |
| Katalogi wxWidgets (`resources/localization/wx_locale`) | wxWindows Library Licence 3.1 | z wxWidgets |
| Kształty galerii (`resources/shapes`: 3DBenchy, box, bunny, nakrętka M3 itd.) | **niezweryfikowane** | w drzewie nie ma notek licencyjnych; 3DBenchy jest u źródła CC BY-ND 4.0 (Creative Tools) — warunek ND i atrybucja do sprawdzenia przed dystrybucją binarną, albo usunięcie modelu |
| `resources/udev` (reguły 3Dconnexion) | — | nie jest instalowane w pakiecie SLA |

## 6. Czego brakuje (do zrobienia przed publiczną dystrybucją)

1. Plik `THIRD_PARTY_NOTICES` w paczce z pełnymi tekstami licencji powyższych komponentów
   (dziś teksty leżą tylko w źródłach `deps/` i w `bundled_deps/`); `tools/bundle_native_libraries.py`
   kopiuje tylko notki OpenCV.
2. Rozstrzygnięcie licencji `resources/shapes` (3DBenchy) i danych profili producentów.
3. Wersja ADMesh, libigl, libnest2d, GLU libtess, semver — brak numerów w źródłach (SBOM: `version` puste).
4. Zależności tranzytywne bibliotek systemowych zależą od dystrybucji; SBOM wylicza je tylko dla
   maszyny, na której uruchomiono generator.
