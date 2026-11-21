# Dokumentacja deweloperska SLA-Slicer

Dla osób rozwijających kod. Użytkownik: [../manual.txt](../manual.txt).
Testy: [TESTING.md](TESTING.md). Zmiany i znane ograniczenia wydania:
[../CHANGELOG.md](../CHANGELOG.md).

SLA-Slicer to PrusaSlicer 2.9.4 zredukowany do SLA/MSLA, offline, z warstwą
edycji pikseli, formatami, PCB i kalibracjami wzorowanymi na UVtools. Nazwa
binarium (`sla-slicer`) jest celowo historyczna (`sla-slicer` to dowiązanie).
Wersja produktu to `SLA_SLICER_VERSION` (1.0.0, `version.inc`); `SLIC3R_VERSION`
(2.9.4) to wersja bazy kodu PrusaSlicer używana do zgodności (pakiety producentów,
nagłówki ini/3MF) i nie zmienia się przy wydaniu. Klucz `SLIC3R_APP_KEY` =
`SLA-Slicer` wyznacza katalog danych (`~/.config/SLA-Slicer`), `SLA-Slicer.ini`,
domenę tłumaczeń (`SLA-Slicer.mo`/`SLA-Slicer_pl.po`) i nazwy D-Bus. Jednorazowy
import części SLA ze starego `~/.config/PrusaSlicer`: `libslic3r/LegacyDataMigration.*`
(tylko kopiuje; test `sla_legacy_migration`, `SLA_SLICER_NO_MIGRATION=1` wyłącza).
Identyfikator kompilacji: `cmake/SlaSlicerBuildId.cmake` (git describe, nadpisanie
`SLA_SLICER_BUILD_ID`, w docker przekazywane z hosta).

## 1. Mapa architektury

```
src/libslic3r/                  silnik (bez GUI)
  SLAPrint*.cpp/.hpp            potok SLA: slicing, podpory, pad, drążenie, raster
  Format/                       kodeki i rejestr formatów
    SLAArchiveFormatRegistry    rejestr: id, opis, rozszerzenie, fabryki writer/reader
    SLAArchiveWriter/Reader     interfejsy kodeków
    SL1, AnycubicSLA, AnycubicZipArchive, ChituboxArchive, CTBEncrypted*,
    CrealityCXDLP*, UVToolsCompatArchive (GOO, PRZ, UVJ, ZIP, CWS...),
    NanoDLP*, Klipper*, Makerbase*, PhrozenArchive, OSF, ...
    *Layout.hpp                 czytniki/przepisywacze natywne (fale 3-8): układ pliku
                                -> maski i z powrotem; patrz rozdz. 4a
  SLA/                          domena SLA (bez GUI)
    LayerDocument               dokument masek warstw: odczyt archiwów, edycje,
                                Undo/Redo, zapis natywny, konwersja
    LayerIssues                 detekcja problemów (port IssueManager UVtools)
    LayerOperations             operacje UVtools na maskach i parametrach (czyste funkcje)
    LayerExports                eksporty warstw: PNG/PGM, GIF, heatmapa, szkielet, HTML, STL
    LayerSuggestions            sugestie UVtools (tylko ocena, nic nie stosuje)
    ProjectArchive, ProjectSettings   projekt warstw .sla-project, 3MF z maskami
    PCBExposure                 Gerber RS-274X / Excellon -> maski
    CalibrationRaster, ExposureFinder*, CalibrationRecipe   kalibracje
    PrinterGeometry, PrinterRaster, RasterBase   LCD -> stół, orientacja, odbicia
    ExportRequest/Snapshot/Generation/Publication   zlecenie i publikacja eksportu
    FreshLayerJob, *FreshExportPlan   dokumenty generowane (kalibracje, PCB)
src/CLI/                        wiersz poleceń (Run.cpp, ProcessActions.cpp)
src/slic3r/GUI/                 interfejs wxWidgets/GTK3
  MainFrame.cpp                 menu, skróty
  Plater.cpp, Sidebar.cpp       scena 3D, panel boczny, przyciski Slice/Export,
                                widoki 3D / Preview / Sliced
  LayerEditorDialog.cpp         edytor Sliced + panel Issues + okno Operations (ok. 3900 wierszy)
  SlaRasterToolsDialog.cpp      okna PCB i kalibracji
  Tab.cpp, Preset*.cpp          strony ustawień drukarki/żywicy/procesu
  ConfigWizard.cpp              kreator pierwszej konfiguracji
tests/                          sla_print (Catch2), oracle, perf, gui, architecture
tools/                          paczka relokowalna, kontrola bibliotek
```

Profile: `Preset.cpp/.hpp`, `PresetBundle.cpp/.hpp` (`PresetFileTransaction`,
usuwanie z zależnościami, import/eksport .ini); GUI w `Tab.cpp`.

Przepływ modelu: scena -> `SLAPrint` (steps) -> rastry warstw -> `LayerDocument`
(wynik w pamięci, edytowalny) -> `ExportRequest` -> writer z rejestru ->
`publish_export` (plik tymczasowy + weryfikacja + rename). Dokumenty bez modelu
(otwarty archiwum, PCB, kalibracje) wchodzą do `LayerDocument` przez
`load_archive` albo `from_generated_rasters`.

Geometria: stół = prostokąt LCD (`sla::synchronize_bed_shape`,
`SLA/PrinterGeometry.hpp`). `display_*` opisują fizyczny, nieobrócony ekran;
orientacja portrait i odbicia działają tylko na piksele pliku, raz, w rasteryzerze.
Nie transformuj masek drugi raz w writerach ("masks already use printer raster
coordinates").

## 2. Budowanie

```sh
bash ./build_linux.sh --run                 # deps/build + build/, test `--help`
bash ./build_linux.sh --jobs 4 --tests --run
bash ./build_linux.sh --clean --deps        # przebuduj zależności
```

Skrypt konfiguruje statyczne zależności (`deps/`, katalog `deps/build`), potem
aplikację: `-DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DSLIC3R_PCH=OFF`, testy
`-DSLIC3R_BUILD_TESTS` i `-DSLIC3R_TEST_SUITES='sla_print;thumbnails'`.
Wymagane: GTK3, OpenCV >= 4.7 core/imgproc (rysowanie ExposureFinder, stackBlur), Python >= 3.9
dla niektórych skryptów QA (nie są w repozytorium). Szczegóły i naprawy zależności:
[../install.md](../install.md), [Dependencies.md](Dependencies.md).

Po dodaniu nowych plików źródłowych uruchom ponownie `cmake` w `build/`.
Przyrostowo: `cmake --build build --target PrusaSlicer -j4` (binarium) i
`--target sla_print_tests`.

### Build przenośny (Docker, glibc 2.35)

Binarium z hosta wymaga glibc hosta (CachyOS: 2.43). Paczka dla innych dystrybucji
powstaje w kontenerze Ubuntu 22.04: `JOBS=8 bash tools/docker/build_portable.sh`
(obraz `tools/docker/Dockerfile.build`, kroki w `tools/docker/container_build.sh`,
katalog roboczy `build-docker/`), także jako `bash build_linux.sh --docker`. Zawartość
`lib/` i wymagania hosta: `install.md`, sekcja 3.

Pułapki znalezione przy tym buildzie (zachowaj przy zmianach):
* Źródła do kontenera: host liczy listę `git ls-files -co --exclude-standard` (zestaw plików
  repozytorium) i kopiuje ją `rsync --files-from`; bez `.git` kopiowane jest całe drzewo, z
  wykluczeniami zakotwiczonymi w katalogu głównym i jawnie włączonym `build-utils/` (definiuje
  funkcję CMake `encoding_check`).
* Część testów wymaga katalogu buildu wewnątrz źródeł (`/work/src/build`).
* OpenJPEG z Ubuntu nie dodaje katalogu nagłówków do celu `openjp2` — dodawany jawnie
  w `src/libslic3r/CMakeLists.txt`.
* libtiff ≤ 4.3 nadpisuje bufor przekazany do `TIFFWriteScanline` (predyktor), dlatego
  `tiff_encode` podaje kopię wiersza (`SLA/LayerImageFile.cpp`; test w `sla_layer_image_tests.cpp`).
* Do `lib/` paczki nie wolno kopiować bibliotek stosu GLib/GTK (np. `libmount` z Ubuntu 22.04
  psuło `libgio` na nowszych systemach) — filtr `HOST_LIBS` w `tools/make_relocatable_bundle.sh`.

## 3. Konwencje

- **Brak adapterów runtime do UVtools i PrusaSlicer.** UVtools jest wyłącznie
  wzorcem testowym uruchamianym z zewnątrz (lokalne skrypty QA, nie w repozytorium); kod produkcyjny nie
  ładuje DLL, nie uruchamia .NET i nie wywołuje obcych programów. Zachowanie
  UVtools portujemy natywnie (np. `LayerIssues`, kodeki), z opisem WHY w komentarzu.
- **Offline.** Aplikacja jest zawsze offline (brak opcji CMake: `SLIC3R_OFFLINE_ONLY`
  i `SLIC3R_SLA_ONLY` usunięto w W11). Kod sieciowy, WebKit/libsoup, wysyłanie druku
  i aktualizacje nie istnieją w źródłach; pilnuj indeksów `m_changeable_menu_items`
  (zostaw pusty slot, gdy usuwasz pozycję menu). Test `sla_offline_no_network_libs`
  sprawdza `readelf -d`. Nie dodawaj tekstów G-code/FFF do interfejsu.
- **Atomowa publikacja.** Każdy zapis pliku wynikowego idzie przez
  `sla::publish_export` (`SLA/ExportPublication.hpp`): plik tymczasowy obok celu,
  weryfikacja odczytem zwrotnym, sprawdzenie anulowania, `rename`. Anulowany lub
  nieudany eksport nie zmienia poprzedniego pliku. CLI nie może zmienić nazwy
  pliku, którego nie zapisało. Projekty warstw i profile (`ProfileFileTransaction`)
  używają tego samego wzorca. Znane braki: brak `fsync`, brak wycofania przy
  błędzie w połowie migracji wielu INI przy zmianie nazwy profilu.
- **Anulowanie.** Długie operacje przyjmują `sla::LayerProgress`
  (`std::function<bool(done,total)>`; `false` = anuluj) i wołają
  `check_layer_progress`, które rzuca `LayerOperationCancelled`. Anulowanie to
  osobny wynik (nie błąd pliku): zostawia maski, historię i poprzedni plik bez
  zmian. Kod równoległy (TBB) sprawdza anulowanie co partię; callbacki muszą być
  thread-safe. W GUI długie zadania idą przez `run_background_layers` z oknem
  postępu i Cancel.
- **Błędy zamiast cichej naprawy.** Writer, który nie zachowa ustawienia
  (odbicie Y, różne prędkości cofania, kwadratowy piksel...), odmawia z
  konkretnym komunikatem przed zapisem pliku (stąd status BLOCKED w sweepie).
  Nigdy nie zapisuj pliku o zmienionej orientacji czy parametrach po cichu.
- **Limity wejścia.** Czytniki sprawdzają długość i wymiary przed alokacją
  (`check_archive_length`); ucięty plik to błąd odczytu, nie przekroczenie budżetu.
- **Styl.** Czytelny kod, komentarze wyjaśniają DLACZEGO (inwarianty, wybór
  UVtools-zgodny), nie CO. Nowy kod bez gęstych jednowierszowców.
  Nowy tekst interfejsu: `_L("...")` (zmiana klucza gubi tłumaczenie).

## 4. Dodawanie formatu eksportu

1. **Kodek writer.** Klasa dziedzicząca po `SLAArchiveWriter`
   (`Format/SLAArchiveWriter.hpp`) w nowym `Format/<Nazwa>Archive.cpp/.hpp`
   (dopisz do `src/libslic3r/CMakeLists.txt`). Maski mają już współrzędne
   rastra drukarki; nie transformuj ich ponownie. Walidacje geometrii i ruchów
   zrób przed zapisem (komunikat zamiast cichej zmiany). Zapis idzie przez
   `publish_export`.
2. **Rejestr.** Dodaj `ArchiveEntry{id, L("opis"), "ext", {aliasy}, fabryka
   writer, fabryka reader lub {}}` w `Format/SLAArchiveFormatRegistry.cpp`.
   Identyfikator jest stabilny (zapisywany w profilach — nigdy opis ani indeks).
   Rodziny ZIP dzielą rozszerzenie, więc odróżniaj je identyfikatorem.
   Rejestr zasila listę w Printer Settings, filtr okna eksportu, CLI
   (`--sla-archive-format`) i sweep oracle.
3. **Czytnik w `LayerDocument`** (jeśli format ma być edytowalny): rozpoznanie w
   `LayerDocument::load_archive` (`SLA/LayerDocument.cpp` — po magicznych bajtach
   lub rozszerzeniu), dekodowanie masek do PNG w pamięci, ustawienie
   `m_archive_format`, zapis natywny przy zapisie edytowanym (zachowaj bajty
   nieznanych pól) i walidacja rozszerzenia przy `Save`. Parametry warstw
   (Z, ekspozycja, ruch) przez `LayerParameters`/capabilities.
   Bez czytnika format pozostaje tylko do eksportu — zapisz to w
   tabeli rozdz. 7 manuala.
4. **Zgodność parametrów.** Jeśli format nie przechowuje jakiegoś ustawienia,
   dopisz je do kontroli zgodności (Material -> Output compatibility) i do
   walidacji drukarki (`SLA/ExportPrinterValidation.hpp`, plany `*FreshExportPlan`).
5. **Testy.** Catch2 w `tests/sla_print/` (round-trip: eksport -> odczyt ->
   te same piksele i metadane; warstwy puste, pełne, długie przebiegi RLE,
   uszkodzony i ucięty plik, anulowanie); dopisz plik do
   `tests/sla_print/CMakeLists.txt` i tag do właściwego testu CTest. Test
   rejestru: `sla_archive_registry_tests.cpp`.
6. **Wzorzec.** Odczytaj plik niezależnym narzędziem (UVtools `UVtoolsCmd`, jeśli ma czytnik tego
   formatu). Jeśli UVtools nie ma czytnika, zapisz to jawnie i napisz własny niezależny dekoder w
   teście. Nie stosuj tolerancji, by ukryć błąd orientacji/odbić. Zaktualizuj tabelę w
   `manual.txt` rozdz. 7. (Skrypty porównujące z UVtools są lokalnym narzędziem QA, patrz
   [TESTING.md](TESTING.md).)
7. **GUI.** Zwykle nic: format pojawia się w liście i oknie eksportu z
   rejestru. Dodatkowe pola (jak `sla_anycubic_version`) wymagają opcji w
   `PrintConfig.cpp`, edytora w `Tab.cpp` i zapisu w projekcie.

## 4a. Moduły dodane w falach 5-8

Wspólna zasada: logika jest w `libslic3r` jako czyste funkcje na prostych
maskach (8 bit) i strukturach wartości, bez GUI, OpenCV i formatów plików;
`LayerDocument` łączy je z Undo/Redo, a dialog tylko zbiera parametry i
uruchamia zadanie w tle. Każdy port UVtools ma w nagłówku tabelę "operacja
UVtools -> funkcja -> odchylenia" (co jest zgodne co do piksela, co ±1 poziom,
czego nie portowano) — aktualizuj ją przy zmianie zachowania.

- **`SLA/LayerOperations.hpp/.cpp`** — operacje UVtools (Move, Resize, Rotate,
  Flip, Pattern, Solidify, Threshold, PixelDimming, Infill, PixelArithmetic,
  RaftRelief, LayerClone/Remove/ReHeight/Arithmetic, DoubleExposure,
  ChangeResolution oraz z fali 7: DynamicLifts, FadeExposureTime,
  LightBleedCompensation, PhasedExposure, RaiseOnPrintFinish, EditParameters,
  Timelapse, DynamicLayerHeight, Lithophane, RepairLayers, Calculator).
  Walidacja przed zmianą, anulowanie przez `LayerProgress` bez częściowych
  wyników. Do dokumentu wchodzą cztery ścieżki w `LayerDocument`:
  `apply_mask_batch` (operacje pikseli, jeden krok Undo),
  `apply_layer_list_operation` (zmiana listy warstw; sprawdza
  `layer_list_refusal` — archiwa otwarte z pliku odmawiają, bo liczba warstw i
  wysokości leżą w natywnym nagłówku), `apply_parameter_operation` i
  `apply_parameter_list_operation` (tabela parametrów warstw; pole, którego
  format nie zapisze, daje odmowę z nazwą pola), plus
  `from_parameter_masks` (nowy dokument, np. Lithophane).
- **`SLA/LayerExports.hpp/.cpp`** — eksporty warstw jako czyste funkcje
  zwracające bajty/maski (własny koder GIF89a/LZW i binarny STL; PNG przez
  koder projektu), a `export_*` zapisują pliki. Eksport nie zmienia
  dokumentu; GUI zapisuje przez `.part` + rename i może anulować.
- **`SLA/LayerSuggestions.hpp/.cpp`** — `evaluate_suggestions(job, config)`:
  wejście to dane (`SuggestionJob`), wynik to `SuggestionResult` (status
  applied/not_applied/unavailable, proponowana zmiana). Brak danych w
  kontenerze = `unavailable`, nigdy wymyślona wartość. Używa tego CLI
  `--sla-suggestions` (`CLI/ProcessActions.cpp`); stosowanie propozycji do
  dokumentu jest pozostawione wywołującemu (`set_layer_parameters` + Undo).
- **`SLA/LayerIssues.hpp/.cpp`** — detekcja problemów (TBB, anulowanie,
  deterministyczna kolejność). Jedyne źródło dla panelu Issues i CLI
  `--sla-detect-issues`; zgodność z UVtools sprawdzano lokalnym skryptem QA.
- **`Format/*Layout.hpp`** — czytniki natywne rodzin z fal 3-8 (patrz 4a.1).
- **`PresetFileTransaction`** (`Preset.hpp/.cpp`) — wielopikowa transakcja plików
  INI: `stage_config` / `stage_rename` / `stage_remove` zapisują do plików
  tymczasowych obok celów, `commit()` publikuje wszystko albo nic (błąd w trakcie
  publikacji przywraca opublikowane pliki z kopii w pamięci). Używają jej
  rename, delete-z-zależnościami i import profilu w `PresetBundle`
  (`stage_renamed_preset_references`, `delete_preset_with_dependencies`,
  `export_preset_ini`, `import_preset_ini`). Stan w pamięci zmieniaj dopiero po
  `commit()`. Haczyk testowy `set_fault_injector` pozwala awarii trafić w każdy z
  zapisów (`[sla_profile_transaction]`). Uwaga: `SLA/ProfileFileTransaction.*` to
  martwy duplikat spoza CMake — nie rozwijaj go.

### 4a.1 Dodawanie czytnika (otwieranie i edycja istniejącego formatu)

Zakładamy, że writer i wpis w rejestrze już istnieją (rozdz. 4). Wzorzec rodzin
binarnych: cały plik zostaje w wpisie 0 dokumentu (zapis bez zmian jest bajtowo
identyczny), maski są dekodowane raz do PNG w pamięci, a zapis przekodowuje tylko
zmienione warstwy.

1. **Nagłówek `Format/<Nazwa>Layout.hpp`** — klasa układu:
   - `static bool is_format(const std::string &id)` (id rejestru, np. `"osla"`),
     `static bool recognizes(const uint8_t *first8)` (sygnatura pierwszych 8 bajtów;
     gdy format nie ma magicznych bajtów, rozpoznawanie oprzyj o rozszerzenie, jak
     OSF/Anet, i potwierdź nagłówkiem);
   - konstruktor `(const std::vector<uint8_t> &bytes, ..., const std::function<void()> &poll)`
     parsujący nagłówek; pola publiczne `format`, `columns`, `rows`, `count`;
   - `std::vector<uint8_t> decode(size_t layer) const` — maska 8-bit w układzie
     rastra drukarki (nie transformuj jej ponownie);
   - `std::vector<uint8_t> rebuild(const std::map<size_t, std::vector<uint8_t>> &edited, ...) const`
     — nowy plik z przekodowanymi tylko warstwami z mapy, resztę bajtów (nieznane pola,
     miniatury) kopiuj; przelicz sumy kontrolne/offsety/statystyki wymagane przez format;
   - opcjonalnie `static uint8_t quantize(uint8_t)` — jaka szarość przeżyje zapis (edytor
     pokazuje to, co zostanie zapisane).
   Użyj pomocników z `Format/BinaryLayout.hpp` (`binary_layout::Reader`: sprawdzane
   odczyty `u8/u16/u32/f32` rzucają `RuntimeError` z nazwą rodziny, gdy offset z pliku
   wychodzi poza plik; `check_raster`; `put32/append32`). Wzorce: `OpenSlaLayout.hpp`
   (bin), `UniformationLayout.hpp`/`VoxeldanceLayout.hpp` (ZIP przez `MemoryZip.hpp`),
   `NanoDLPLayout.hpp`/`KlipperLayout.hpp`/`AnycubicZipLayout.hpp` (ZIP + JSON).
   Wszystkie długości i wymiary waliduj przed alokacją (`check_archive_length`);
   ucięty plik to błąd odczytu.
2. **Podłączenie w `SLA/LayerDocument.cpp`** (patrz komentarz nad
   `is_native_binary_layer_format`): dopisz rodzinę do `is_native_binary_layer_format`
   i `quantize_native_binary`, dodaj sondę w `load_archive_impl` (obok LGS/OSF/Anet/
   OSLA... przez `import_binary` albo wariant ZIP), dodaj gałąź w
   `save_native_binary_dispatch` (`save_native_binary<Layout>` albo
   `save_native_zip<Layout>`) i regułę rozszerzenia przy `Save` (rozszerzenie
   musi odpowiadać otwartemu kontenerowi). Formaty wektorowe (jak SVGX) mogą być
   tylko do podglądu: `save` odmawia z `read_only_reason()` dla zmienionego dokumentu.
3. **Parametry warstw** (opcjonalnie): rodzina, której kontener naprawdę
   przechowuje wartości per warstwa, dopisuje się do listy `NativeFamily` w
   `LayerDocument.cpp` (kanonizacja jednostek, `validate_native_row` — te same
   reguły, które egzekwuje writer, by edycja nie wygenerowała pliku odrzucanego
   przez eksporter). Bez tego okno Operations/Layer parameters odmawia z nazwą pola
   i tabela w manualu ma "nie". Statystyki w nagłówku (objętość, czas) przelicz
   albo jawnie zapisz, że są kopiowane.
4. **Testy** — nowy plik `tests/sla_print/sla_<nazwa>_tests.cpp` z tagami
   `[sla_layers][sla_more_readers]` (lub kolejny tag rodziny; `[sla_layers]` wciąga plik
   do CTest `sla_layer_document`): odczyt pliku z własnego eksportera (te same piksele i
   metadane), zapis bez zmian bajtowo identyczny, edycja jednej warstwy zmienia tylko
   ją, uszkodzone i ucięte wejście, anulowanie, odmowa niezapisywalnych pól. Dopisz plik
   do `tests/sla_print/CMakeLists.txt`. Pikselową zgodność z wzorcem sprawdź ręcznie:
   `UVtoolsCmd extract` na pliku po zapisie edycji.
5. **Dokumentacja** — kolumny "Maski"/"Parametry" w tabeli rozdz. 7 `manual.txt`,
   `CHANGELOG.md` i `doc/THIRD_PARTY_LICENSES.md`, jeśli format dołączył kod obcej licencji.

### Layer editor source map

Edytor warstw (`src/slic3r/GUI/`) jest podzielony na jednostki translacji bez zmiany zachowania; publiczne API to nadal tylko `LayerEditorDialog.hpp`. Prywatne typy żyją w `Slic3r::GUI::layer_editor_detail`.

- `LayerEditorDialog.cpp` — cienkie wejście: `create_sliced_layer_editor`, `create_layer_file_editor`, `choose_layer_file`, wczytywanie pliku (`load_layer_file`).
- `LayerEditorInternal.hpp` — prywatny nagłówek: wspólne includy, `TaskLock`, `LayerOpsJob`/`LayerOpsHost`, `OpField`/`OpValues`/`OpContext`/`BuiltOp`/`OpDef`, helpery `ops_detail`/`batch2_detail`, deklaracje funkcji współdzielonych.
- `LayerEditorCommon.cpp` — `set_dataview_accessible_name` (nazwa dostępności GTK dla wxDataView), `issue_type_label`, `issue_layer_positions_z`.
- `LayerOperationCatalog.cpp` / `LayerOperationCatalog_Batch2.cpp` — tabela operacji (`make_layer_operation_defs`, `append_batch2_defs`): formularze, domyślne UVtools i buildery.
- `LayerOperationsDialog.cpp` — dialog operacji (podgląd/Apply), `create_layer_operations_dialog`; `LayerCalculatorDialog.cpp` — kalkulator (`show_layer_calculator`).
- `LayerEditorContent.hpp` — deklaracja panelu Sliced (`LayerEditorContent`); definicje (fala 27 rozbiła jeden ~970-wierszowy konstruktor z lambdami na nazwane metody): `LayerEditorContent.cpp` (krótki konstruktor + `build_paint_controls`, `build_preview_canvas`, `build_range_controls`, `build_range_filters`, `build_action_buttons`/`add_action_button`, `build_edit_buttons`, `build_format_section` → `build_export_format_controls` / `build_profile_controls` / `build_archive_note`, `build_project_buttons`; kolejność tworzenia widżetów jak dawniej), `_Events.cpp` (`bind_events` i uchwyty płótna: `on_preview_left_down/motion/wheel/key_down`, `on_char_hook`), `_Export.cpp` (`run_export_action`, `export_through_print`, `export_native_archive`, `convert_archive`, `export_mask_images`, `update_format_note`), `_Dialogs.cpp` (`apply_range_operation`, okna receptury kalibracji i ustawień projektu, `capture_selected_settings`, podsumowanie profilu żywicy), `_Canvas.cpp` (podgląd, zoom/pan, pociągnięcia, historia), `_Operations.cpp` (uruchamianie operacji/eksportów, host operacji, zapis projektu, parametry warstw), `_Issues.cpp` (panel i detekcja problemów, nakładka).

## 5. Zasady pracy z kodem

- **Nie czytaj `LayerEditorDialog.cpp` w całości** (ok. 1800 gęstych wierszy).
  Szukaj `grep -n` po etykiecie przycisku/nazwie kontrolki i czytaj okno
  +-60 wierszy. Etykiety: `grep -noE '"[A-Z][^"]{3,60}"'`. To samo dotyczy
  `Plater.cpp`, `Tab.cpp`, `MainFrame.cpp`, `LayerDocument.cpp` (3500 wierszy)
  i `PrintConfig.cpp`.
- Nowa logika edycji idzie do `SLA/` (testowalna bez GUI), nie do dialogu;
  dialog tylko zbiera parametry i uruchamia zadanie w tle.
- Menu: `MainFrame.cpp` (`generate_help_menu`, bloki `append_menu_item`);
  skróty także w `KBShortcutsDialog.cpp` — aktualizuj oba i unikaj kolizji
  (Ctrl+Alt+L = Edit sliced layers, Ctrl+Shift+L = Open layer archive,
  Ctrl+Shift+P = Import Config from Project).
- Zmiany interfejsu nie są objęte CTest: po każdej uruchom ręczną listę z
  `doc/TESTING.md` rozdz. 4 albo oznacz je jako nieprzetestowane.
- Nie zmieniaj ręcznie plików poza swoim zakresem; stanu wymagań nie edytuj
  w trakcie pracy nad kodem — raportuj go orkiestratorowi.
- Zależności i licencje: nowa biblioteka z `deps/` lub `bundled_deps/` wymaga
  wpisu w `tools/generate_sbom.py` (licencja SPDX, sposób linkowania) i w
  `doc/THIRD_PARTY_LICENSES.md`; po zmianie uruchom `python3 -I tools/generate_sbom.py`.
  Kod portowany z UVtools zostaje w AGPL-3.0 z uznaniem autora (Tiago Conceição,
  sn4k3) — opisz port w komentarzu nagłówka.
- Nie komituj artefaktów: `build/`, `dist/`, katalogów `fresh-*`/`tilt-target-*`,
  logów i próbek eksportu.
