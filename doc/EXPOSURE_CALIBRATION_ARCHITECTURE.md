# Natywne kalibracje: granice modułów i testowanie

Dokument opisuje aktualny przepływ kodu. Znane ograniczenia wydania:
[CHANGELOG](../CHANGELOG.md). Opis architektury nie oznacza ukończenia całej suite.

| Odpowiedzialność | Moduł | Kontrakt |
|---|---|---|
| Szkic formularza i kontrolki | `src/slic3r/GUI/ExposureFinderDialog.cpp` | Przechowuje surowy tekst; nie zmienia profilu przy wpisywaniu. |
| Parsowanie i plan | `src/libslic3r/SLA/ExposureFinderDraft.*` | Czyste parsowanie; plan używa tych samych algorytmów i walidacji co produkcja. |
| Tabela prób | `SLA/ExposureFinderTable.*` | Kolejność i duplikaty są jawne; normalizacja to osobna operacja. |
| Parametry i harmonogram | `SLA/ExposureFinderTable.hpp`, `ExposurePositionSchedule.hpp` | Fizyczne wysokości i rekordy naświetlania są różnymi pojęciami. |
| Wzorzec i kompozycja LCD | `SLA/ExposureFinderPattern.*`, `ExposureFinderRaster.*` | Renderuje komórkę, następnie rozmieszcza ją w fizycznej geometrii LCD. |
| Uruchomienie w tle | `src/slic3r/GUI/RasterGeneratorTask.hpp` | Worker nie dotyka wx; callback przekazuje postęp i sprawdza anulowanie. |
| Publikacja wyniku | `GUI/Plater.cpp`, `SLAPrint` | Nowy dokument zastępuje poprzedni dopiero po zakończeniu generowania i zgodzie na utratę edycji. |
| Persystencja | `SLA/ProjectArchive.cpp`, `CalibrationRecipe.hpp` | Receptura jest historią generowania; nie regeneruje później edytowanych masek. |
| Paleta i eksport | `GeneratedLayerOwnership.hpp`, `LayerMaskPrecision.hpp`, `Format/*` | Możliwości pikseli i zachowanie parametrów są oddzielnymi kontraktami. |

Ścieżki `SLA/*` w tabeli są względem `src/libslic3r/`.

## Szkic, preview i generowanie

Formularz zachowuje także nieaktywne wartości. Parsery rozmiarów cech mogą
sortować i usuwać powtórzenia; lista brightness zachowuje kolejność, powtórzenia
i zero. Ręczne wiersze zachowują dokładne double, kolejność, duplikaty i nieważne
pozycje. Nie należy dodawać cichej normalizacji do parserów ani eventów GUI.

Preview kopiuje request i konfigurację. Waliduje efektywne parametry ruchu,
renderuje dwie płaszczyzny komórki i sprawdza rzeczywiste rozmieszczenie. Nie
alokuje kompletnego stosu LCD. Podawana liczba logicznych bajtów nie jest
estymacją pamięci szczytowej ani rozmiaru archiwum.

Zmiana pól unieważnia preview. Generate wymaga zgodności fingerprintu szkicu
i konfiguracji z zaakceptowanym planem. Nowe ustawienie wpływające na maski lub
parametry musi wejść do snapshotu, fingerprintu, typed recipe i testów roundtrip.
Nie wolno dodawać pola wyłącznie do widoku.

## Anulowanie i publikacja

`run_raster_task` działa na wątku GUI i uruchamia producenta przez `std::async`.
Stan atomowy callbacku żyje dłużej niż future. Cancel, Escape i zamknięcie okna
żądają anulowania; modal pozostaje żywy do zakończenia workera. `get()` łączy
wątek przed zniszczeniem callbacku. Inne wyjątki są propagowane po połączeniu.

Anulowanie jest kooperacyjne. Nowe kosztowne pętle muszą wywoływać checkpointy.
Samo użycie helpera nie gwarantuje szybkiego Cancel: prosta kalibracja nadal ma
zbyt mało wewnętrznych checkpointów. Nie deklarować opóźnienia na podstawie
jednego testu lub czasu samego zamknięcia okna.

Po Cancel lub odmowie publikacji należy zachować stary dokument, jego historię
i wypełniony szkic. Nie publikować częściowego stosu ani zerować historii starego
wyniku. Test GUI musi sprawdzić rzeczywisty dokument po tej operacji.

## Paleta a parametry pliku

NativeGray4 oznacza dokładną paletę `17 * floor(value / 16)`. Dla Anycubic ZIP
projekcja odbywa się przed pierwszym PNG, również na pikselach AA. Plan, receptura,
projekt i wynik PW0 muszą wtedy przedstawiać te same effective levels.

Znajomość palety nie uprawnia do utworzenia fresh descriptor. Ten descriptor
obowiązuje dopiero po wdrożeniu zachowującego parametry writer contract. Historyczna
receptura może istnieć bez niego; zmiana formatu docelowego nie zmienia jej palety.
Walidacja persystencji dopuszcza obecnie Gray8/NativeGray4 dla takiego legacy
źródła, nadal wymaga poprawnej receptury, wygenerowanego pochodzenia i ustawień.
Dla istniejącego fresh descriptor producent i precyzja muszą zgadzać się dokładnie.

## Bramka nowej zmiany

1. Odtwórz błąd na rzeczywistym wejściu i zachowaj RED. Testuj zachowanie, a nie
   przepisane linie implementacji. Oddziel błąd selektora GTK od błędu produktu.
2. Uruchom odpowiednie testy C++: draft, receptura, harmonogram, wzorzec lub
   kalibracja. Zachowaj fixture eksportera i projektów, jeśli zmiana dotyczy pliku.
3. Sprawdź rzeczywiste GUI: wpisanie pól, odświeżenie planu, Cancel/No/Yes,
   edycję, zapis i restart. Wywołanie fabryki nie zastępuje testu kontrolki.
4. Porównaj dekodowane piksele, geometrię i parametry z oryginalnym UVtools.
   QA może używać lokalnego .NET/Pillow; produkcja nie może od nich zależeć.
5. Przypnij SHA źródeł, binariów i fixture. Pełny CTest wykonaj dla końcowego stanu.
   Wyniki wcześniejszego binarium pozostają wynikami historycznymi.

Skrypty QA GUI i oracle (formularz Exposure Finder, ręczne scenariusze, palety ZIP) są lokalnym
narzędziem i nie należą do repozytorium (patrz [TESTING.md](TESTING.md)). Wszystkie katalogi
wyników i TMPDIR powinny być w projekcie. Benchmark uruchamiać osobno.
