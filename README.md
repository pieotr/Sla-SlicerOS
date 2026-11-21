# SLA-Slicer

SLA-Slicer 1.0.0 to slicer żywiczny (SLA/MSLA, drukarki z ekranem LCD) oparty na
kodzie PrusaSlicer 2.9.4, ograniczony do druku SLA i działający w całości offline.
Nie ma G-code, druku FFF/FDM ani funkcji sieciowych (wysyłanie na drukarkę, aktualizacje,
konta).

SLA-Slicer nie jest powiązany z Prusa Research. PrusaSlicer jest znakiem/produktem
Prusa Research; nazwa jest tu użyta wyłącznie do wskazania pochodzenia kodu.

## Najważniejsze funkcje

- Slicing modeli 3D (STL, OBJ, 3MF) na warstwy-maski z podporami, wydrążaniem, padem
  i podglądem warstw.
- Zapis w 80 identyfikatorach formatów drukarek (SL1/SL1S, Anycubic, CTB/CBDDLP/Photon,
  GOO, CXDLP, Klipper, NanoDLP, ZCode i inne; pełna lista w
  [doc/COMPATIBILITY_INVENTORY.md](doc/COMPATIBILITY_INVENTORY.md)).
- Edytor warstw (widok Sliced): pędzel, gumka, zakresy warstw, operacje na maskach,
  wykrywanie problemów (wyspy, nawisy, pułapki powietrza, przyssawki), Undo/Redo,
  zapis do pliku drukarki.
- Ekspozycja PCB z plików Gerber i Excellon (maska, miedź, otwory).
- Kalibracje odpowiadające UVtools (Exposure Finder, tabele ekspozycji, wzorce
  testowe) liczone natywnie w C++.
- Tryb wsadowy i automatyzacja: CLI (`sla-slicer --help-sla`), JSON z wynikami,
  kody wyjścia — [doc/AUTOMATION.md](doc/AUTOMATION.md).
- Interfejs po polsku i po angielsku.

Stan i ograniczenia wersji: [CHANGELOG.md](CHANGELOG.md).

## Instalacja

Gotowe pakiety dla Linuksa x86_64 (glibc 2.35 lub nowszy, GTK3, OpenGL):

- AppImage: `chmod +x SLA-Slicer-1.0.0-x86_64.AppImage && ./SLA-Slicer-1.0.0-x86_64.AppImage`
- Debian/Ubuntu: `sudo apt install ./sla-slicer_1.0.0_amd64.deb`
- Fedora: `sudo dnf install ./sla-slicer-1.0.0-1.x86_64.rpm`
- Przenośne archiwum: `tar xzf sla-slicer-1.0.0-linux-x86_64.tar.gz` i uruchomienie
  `sla-slicer-1.0.0/sla-slicer`.

Szczegóły (wymagania, pakiety natywne, odinstalowanie): [install.md](install.md).

## Budowanie ze źródeł

Linux (zalecane, GTK3):

```sh
bash ./build_linux.sh --run          # sprawdza wymagania, buduje zależności i aplikację
bash ./run_sla_slicer.sh             # uruchomienie
bash ./build_linux.sh --help         # opcje: --tests, --jobs N, --bundled-opencv, --docker, ...
```

`--docker` buduje przenośne archiwum w kontenerze Ubuntu 22.04. Wymagania pakietowe
dla Debiana/Ubuntu, Fedory i Archa oraz opis OpenCV: [install.md](install.md).

Windows: `build_win.bat` — **EKSPERYMENTALNE, niesprawdzone** (autor nie miał
kompilatora MSVC; skrypt jest dostosowaną wersją skryptu PrusaSlicera).

## Dane użytkownika

Konfiguracja leży w `~/.config/SLA-Slicer` (plik `SLA-Slicer.ini`), oddzielnie od
zainstalowanego PrusaSlicera. Przy pierwszym uruchomieniu, jeżeli nowy katalog nie
istnieje, a istnieje `~/.config/PrusaSlicer`, jego część SLA (profile drukarek,
żywic i procesów, kształty, stan żywic) jest KOPIOWANA do nowego katalogu; katalog
PrusaSlicera nie jest zmieniany. Wyłącza to `SLA_SLICER_NO_MIGRATION=1`.
Osobny katalog do prób: `sla-slicer --datadir /sciezka`.

## Dokumentacja

- [Instrukcja użytkownika](manual.txt) (po polsku): profile, model, slicing, edytor warstw,
  formaty, PCB, kalibracje, CLI, rozwiązywanie problemów
- [Budowanie i instalacja](install.md)
- [Lista zmian i znane ograniczenia](CHANGELOG.md)
- [Dokumentacja deweloperska](doc/DEVELOPER.md), [testowanie](doc/TESTING.md),
  [automatyzacja](doc/AUTOMATION.md), [edytor warstw](doc/LAYER_EDITOR.md),
  [PCB](doc/PCB_CALIBRATION.md), [architektura kalibracji](doc/EXPOSURE_CALIBRATION_ARCHITECTURE.md),
  [tłumaczenia](doc/Localization_guide.md), [zależności](doc/Dependencies.md)
- [Licencje komponentów](doc/THIRD_PARTY_LICENSES.md) i [SBOM](doc/SBOM.json) (CycloneDX 1.5)

## Licencja

AGPLv3 (patrz [LICENSE](LICENSE)), tak jak PrusaSlicer, z którego pochodzi kod.
Algorytmy UVtools przeniesione natywnie do C++ (wykrywanie problemów, operacje na
warstwach, eksporty, kalibracje, kodeki) pozostają na licencji AGPL-3.0 z uznaniem
autora: Tiago Conceição (sn4k3) i współautorzy UVtools, <https://github.com/sn4k3/UVtools>;
w aplikacji nie ma kodu ani procesu UVtools. Biblioteki zewnętrzne (Boost, wxWidgets,
OpenVDB, CGAL, GMP/MPFR, OpenCV i inne) mają własne licencje:
[doc/THIRD_PARTY_LICENSES.md](doc/THIRD_PARTY_LICENSES.md).
