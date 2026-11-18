# SLA-Slicer

Slicer żywiczny (SLA/MSLA, ekran LCD) oparty na PrusaSlicer 2.9.4, ograniczony
do SLA i działający w całości offline. Tnie model 3D na warstwy-maski, pozwala
poprawić je pikselowo (widok Sliced, wykrywanie problemów), generuje warstwy
PCB z Gerberów oraz kalibracje i zapisuje pliki w formatach drukarek
(80 identyfikatorów formatów w rejestrze). Nie ma G-code, druku FFF/FDM ani
funkcji sieciowych.

Stan: projekt w trakcie przebudowy, gałąź `testing`; żaden z celów G1-G15 nie
jest jeszcze w pełni ukończony. Żaden format nie został sprawdzony na drukarce;
zgodność z UVtools 7.0.0 jest sprawdzana niezależnym odczytem (bramka z 12.11.2026:
402 PASS, 42 przypadki, w których wzorzec jest wadliwy lub nie ma czytnika —
CWS, CXDLP v1, SL1SVG — rozstrzygnięte niezależnymi dekoderami, 116 celowych
odmów writerów, 0 prawdziwych defektów). Zmiany interfejsu z ostatnich prac
(okna Issues i Operations, narzędzia płótna, PCB, kalibracje, profile) mają
testy logiki, ale nie były jeszcze sprawdzone ręcznie w działającym programie.

## Szybki start (Linux, GTK3)

```sh
bash ./build_linux.sh --jobs 4 --run     # zależności + aplikacja, test --help
bash ./run_prusa_slicer.sh               # uruchomienie
bash ./run_prusa_slicer.sh --datadir /sciezka/do/proby   # z osobnym katalogiem danych
```

Plik wykonywalny: `build/src/prusa-slicer` (dowiązanie `sla-slicer`). Dane
użytkownika zostają w katalogu `PrusaSlicer` (nazwa historyczna, celowo
niezmieniona).

## Dokumentacja

- [Instrukcja użytkownika (po polsku)](manual.txt) — profile, model, slicing,
  edytor Sliced, formaty, PCB, kalibracje, CLI, rozwiązywanie problemów, ograniczenia
- [Budowanie i instalacja](install.md)
- [Dokumentacja deweloperska](doc/DEVELOPER.md) — architektura, konwencje, dodawanie formatu
- [Testowanie](doc/TESTING.md) — CTest, oracle UVtools i polityka kodów wyjścia, benchmark, testy GUI
- [Licencje komponentów](doc/THIRD_PARTY_LICENSES.md) i [SBOM](doc/SBOM.json) (CycloneDX 1.5,
  generowany przez `python3 -I tools/generate_sbom.py`)
- [Stan wymagań i znane braki](doc/TODO_STATUS.md) — jedyne bieżące źródło stanu
- [Cel projektu](doc/GOAL.md)

Pozostałe pliki w `doc/` (raporty `M2_*`, `*_2026-10-*`, audyty) to historia prac,
nie opis stanu bieżącego.

## Testy

```sh
bash ./build_linux.sh --tests --jobs 4
ctest --test-dir build --output-on-failure -L smoke
UVTOOLS_CMD=/sciezka/do/UVtoolsCmd ctest --test-dir build -L oracle   # opcjonalnie
```

Szczegóły: [doc/TESTING.md](doc/TESTING.md).

## Licencja

AGPLv3 (patrz `LICENSE`), tak jak PrusaSlicer, z którego pochodzi kod.
Algorytmy UVtools przeniesione natywnie do C++ (wykrywanie problemów, operacje na
warstwach, eksporty, sugestie, kalibracje, kodeki) pozostają na licencji AGPL-3.0
z uznaniem autora: Tiago Conceição (sn4k3) i współautorzy UVtools,
<https://github.com/sn4k3/UVtools>; w aplikacji nie ma kodu ani procesu UVtools.
Biblioteki zewnętrzne (Boost, wxWidgets, OpenVDB, CGAL, GMP/MPFR, OpenCV i inne)
mają własne licencje: [doc/THIRD_PARTY_LICENSES.md](doc/THIRD_PARTY_LICENSES.md).
