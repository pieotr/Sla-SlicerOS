# Testowanie SLA-Slicera

## Co jest w repozytorium

Repozytorium zawiera **wyłącznie testy jednostkowe C++ (Catch2) uruchamiane przez CTest**. Wszystko
inne, czego używano do odbioru (skrypty GUI, odczyt niezależny w UVtools i dekoderach, testy
paczek/dystrybucji, benchmarki, strażnicy architektury w Pythonie, skrypty akceptacyjne, dziennik
statusu) to **lokalne narzędzia QA, które nie są dostarczane w repozytorium** (są ignorowane przez
`.gitignore`). Czysty klon konfiguruje się, buduje i przechodzi wszystkie testy opisane niżej bez
żadnego z nich.

Źródła: `tests/libslic3r`, `tests/sla_print`, `tests/arrange`, `tests/thumbnails`,
`tests/gui_window_geometry`, `tests/gui_background_events`, `tests/cpp17` (budowany tylko na żądanie),
dane testowe `tests/data`. Instrukcja użytkownika: [../manual.txt](../manual.txt), opis kodu:
[DEVELOPER.md](DEVELOPER.md).

Zasady: polecenia uruchamiaj z katalogu głównego repozytorium, ale **pliki wykonywalne testów
uruchamiaj z katalogu budowania** (niektóre testy zapisują pliki w bieżącym katalogu). Nigdy nie
używaj zwykłego katalogu danych użytkownika do prób — zawsze `--datadir` na osobny katalog.

## Budowanie i uruchamianie

```sh
bash ./build_linux.sh --tests --jobs 4          # SLIC3R_BUILD_TESTS=ON, buduje sla_print_tests, uruchamia podzbiór "smoke"
```

W już skonfigurowanym katalogu `build/` (konfiguracja jak w `build_linux.sh`):

```sh
cmake --build build -j8                                  # aplikacja i wszystkie zestawy testów
ctest --test-dir build --output-on-failure -j8           # wszystkie zarejestrowane testy
ctest --test-dir build -L smoke --output-on-failure      # szybki podzbiór
ctest --test-dir build -N                                # lista testów bez uruchamiania
```

Zestawy wybiera `-DSLIC3R_TEST_SUITES='libslic3r;arrange;sla_print;thumbnails'`. Testy
rejestrowane w czystym klonie (22 w CTest): `arrange_tests`, `thumbnails_tests`, `libslic3r_tests`,
`sla_print_tests` (pełny zestaw Catch2 SLA), `sla_window_geometry`, `sla_background_events` oraz
wyspecjalizowane podzbiory `sla_print_tests` pod osobnymi nazwami (poniżej).

Etykiety (z `tests/sla_print/CMakeLists.txt`):

| Etykieta | Co obejmuje |
|---|---|
| `smoke` | szybkie testy regresyjne: rejestr formatów, odczyt/zapis wzorcowy, profile drukarek, dokument warstw, PCB i kalibracje, API offline, migracja starych projektów, zrzut schematu konfiguracji |
| `sla`, `formats`, `layers`, `profiles`, `pcb`, `calibration`, `offline`, `resin`, `migration` | podział tematyczny tych samych testów |
| `geometry`, `diagnostic` | profile wysp/podpór (wolniejsze, każdy w swoim środowisku) |
| `unit`, `gui`, `events` | `sla_window_geometry`, `sla_background_events` — bez ekranu i bez renderera |
| `memory` | `sla_layer_large_documents` (duże dokumenty warstw, limit 300 s) |

Testy `sla_island_full_clip_profile`, `sla_support_link_full_profile` i `sla_support_link_profile_policy`
uruchamiają ten sam plik wykonywalny pod odrębnymi zmiennymi środowiska (`SLA_EXPERIMENT_*`,
`SLA_PROFILE_*`); główny `sla_print_tests` jest uruchamiany bez nich. `-DSLA_SLOW_LAYER_TESTS=ON`
dodaje test kontenera większego niż 1 GiB (minuta, kilka GiB RAM i dysku).

## Catch2: pojedyncze tagi

Główny plik: `build/tests/sla_print/sla_print_tests`. Pojedynczy tag:
`build/tests/sla_print/sla_print_tests "[sla_goo]"`; lista tagów: `sla_print_tests --list-tags`.
Przykłady: `[sla_goo]`, `[sla_goo_reader]`, `[sla_layer_issues]`, `[sla_pcb]`, `[sla_calibration]`.
Ukryte przypadki (nazwa z kropką, np. `[sla_archives]`) tworzą duże pliki i uruchamia się je świadomie, w
osobnym katalogu.

| Tag | Co sprawdza |
|---|---|
| `[sla_layers]` | dokument warstw, pędzel, operacje, Undo, zapis/odczyt (CTest `sla_layer_document`) |
| `[sla_brush]` | pędzel, kształty, przycinanie, matematyka zoomu |
| `[sla_layerops]`, `[sla_layerops2]` | operacje UVtools na maskach i parametrach, walidacja, anulowanie |
| `[sla_layerexport]` | eksporty warstw (PNG/PGM, GIF, heatmapa, szkielet, HTML, STL) |
| `[sla_suggestions]` | sugestie (`LayerSuggestions`) i JSON z CLI |
| `[sla_layer_issues]` | wykrywanie problemów (wyspy, nawisy, pułapki, przyssawki) |
| `[sla_more_readers]`, `[sla_more_readers2]` | czytniki rodzin formatów (Anycubic ZIP, Klipper, NanoDLP, PHZ, FDG, ZCode, LGS, JXS, VDT, ...) |
| `[sla_profile_transaction]`, `[sla_profile_delete]`, `[sla_profile_import_export]` | transakcje i import/eksport profili |
| `[sla_resin_stock]`, `[sla_offline]`, `[sla_legacy_migration]`, `[sla_header_mirror]` | stan żywic, API offline, migracja danych z PrusaSlicera, lustrzane nagłówki |

Dodając test: nowy plik dopisz do `tests/sla_print/CMakeLists.txt`, a tag do właściwego testu CTest
(np. `[sla_layers]` w `TEST_CASE`).

## Lokalne narzędzia QA (poza repozytorium)

Nie są częścią wydania i nie są wymagane do budowania ani testowania. Jeśli masz je w katalogu
roboczym, `tests/CMakeLists.txt` i `tests/sla_print/CMakeLists.txt` rejestrują je automatycznie
(rejestracja jest chroniona przez `if(EXISTS ...)`), z etykietami `oracle` i `gui-runtime`;
domyślny szybki wybór ich pomija:

```sh
ctest --test-dir build -LE "oracle|gui-runtime"
```

Rodzaje: skrypty GUI w prywatnym, niewidocznym pulpicie (KWin/Xwayland/AT-SPI), niezależny odczyt
eksportów przez `UVtoolsCmd` (kod wyjścia 77 = pominięty), testy paczek w kontenerach, benchmarki,
skrypty pilnujące architektury (np. brak bibliotek sieciowych w binarium) i skrypty akceptacyjne.
CTest nie dowodzi zgodności z drukarką: żaden format nie został sprawdzony na urządzeniu.
