## Bieżący stan — 8 listopada 2026

**Najnowszy odbiór R08/R09/R19 — [zgodność profili](M2_PROFILE_COMPATIBILITY_2026-11-08.md).**
Wdrożono All / Selected profiles / Condition / None dla materiał→drukarka,
materiał→proces i proces→drukarka. Błędny warunek oznacza niezgodność i ma
widoczną diagnostykę; pusty zatwierdzony wybór oznacza None, Cancel zachowuje
ustawienia. GUI: zapis, trzy ograniczenia None w projekcie i restart, powrót
do All, rzeczywisty slicing i eksport oraz regresja rename/delete **PASS**.
**CTest19/19, profile33/928; UVtools11 plików/33 maski**, bezpośrednio
18 masek SL1 i 6 masek dwóch projektów. Sześć zwykłych Quit0/INET0;
aplikacja `e3d2f115…`, launcher52 aktualny. Pełny CRUD, migracja powiązań
po rename/delete, odzyskiwanie transakcji i import/export pozostają otwarte.

**Poprzedni odbiór R07/R08/R09 — [zmiana nazwy profili](M2_PRESET_RENAME_LIFECYCLE_2026-11-08.md).**
Naprawiono utratę niezapisanych ustawień po rename, również przy błędzie plikowym.
Operację przeniesiono do `PresetCollection`; GUI zachowuje draft i właściwy wybór.
Drukarka/materiał/proces, nazwy UTF-8, Save, preferencja formatu, Delete No/Yes,
odtworzenie drukarki oraz restart: **GUI PASS**. UVtools **11 plików/33 warstwy**,
bezpośrednio 18 masek SL1 i 3 maski projektu; **CTest19/19**, profile31/868.
Pięć Quit0/INET0; aplikacja `d65b3def…`, launcher51 aktualny. Odbiór obejmuje
Linux GTK, SL1/CXDLPv4; dziedziczenie, import i pełne CRUD pozostają otwarte.

**Poprzedni odbiór R04/R05/R12 — [mieszany zakres Main/SUBIMGS](M2_MIXED_MASK_RANGE_WORKFLOW_2026-11-08.md).**
GUI RED→GREEN: opis powiązań dotyczy zaznaczonego zakresu, również gdy bieżąca
warstwa leży poza nim. Dwa pełne przebiegi Main/SUBIMGS: pędzel, gumka,
Undo/Redo, zapis/restart i podmiana No/błąd/sukces. **14 projektów/84 maski**;
UVtools **8 plików/24 maski główne**, dodatkowy dekoder **18 SUBIMGS**.
**CTest19/19**, cztery Quit0/INET0; aplikacja `7cdfd5ce…`,
natywny silnik/testy bez zmiany, launcher48 aktualny. Zakres PWC518/Linux GTK;
pozostałe R04/R05/R12, wydajność i pełny refaktor nadal otwarte.

**Poprzedni odbiór R01/P0 — [zaokrąglanie i projekcja Clipper](M2_CLIPPER_ROUND_PROJECTION_2026-11-08.md).**
Cały slicing ramki: **22.323→20.095 s
(-9.98%)**, osiem prób ABBABAAB, oba bloki poprawione,
RSS -0.49%. Ponad 10,5 mld porównań zaokrągleń
i 5,29 mld projekcji bez różnic;
213 masek i 452 podpory zgodne. Kontrakty27/186177, trzy małe modele,
**CTest19/19**, GUI2; UVtools łącznie **16 plików / 2214 warstw**.
Cztery Quit0/INET0; aplikacja `48e2ce70…`, launcher48 aktualny.
Dalsza optymalizacja R01 i pełny refaktor pozostają otwarte.

**Poprzedni odbiór R02/P0 — [plan eksportu CXDLPv4](M2_CXDLPV4_VALIDATED_PLAN_2026-11-08.md).**
Cały eksport z edytora dla ramki: **3.042→2.246 s
(-26.18%)**, osiem prób ABBABAAB; oba bloki poprawione,
RSS -1.29%. 213 masek/452 podpory bez zmian.
Zweryfikowane statystyki nie są ponownie liczone; edycje nadal przechodzą
pełną walidację. Kontrakty19/10207, dodatkowe polityki10/8962 i10/9240,
**CTest19/19**, GUI2, UVtools łącznie **16 plików/1350 warstw**,
cztery Quit0/INET0. Aplikacja `67c45be8…`, launcher47 aktualny.
Pierwszy wariant SIMD-only odrzucony; pełny refaktor, R01 i reszta R02 otwarte.

**Poprzedni odbiór R11/R12/P0 — [zapis po odświeżeniu, podczas pracy i po Cancel](M2_MODEL_PROJECT_RUNTIME_SAVE_2026-11-08.md).**
Dwa błędy GUI RED→GREEN: utrata powiązania z Apply po odświeżeniu oraz
ponowne dołączenie masek mimo decyzji o ich pominięciu. **GUI11 PASS;
31 projektów/1596 masek, UVtools22 eksporty/1078 warstw, CTest19/19
(86,42 s), siedemnaście Quit0/INET0.** Dwa duże przypadki: 256 warstw
8192×4096, zapis przy aktywnym workerze i po rzeczywistym Cancel.
Aplikacja `0601cc38…`, silnik bez zmian, launcher47 aktualny.
Zakres: CXDLPv4 Main/Linux GTK. Podmiana sesji, pozostałe formaty/platformy,
wydajność R01/R02 i pełny refaktor pozostają otwarte.

**Poprzedni odbiór R11/R12/P0 — [niezapisane Apply po zmianie metadanych](M2_MODEL_PROJECT_UNSAVED_METADATA_2026-11-08.md).**
Zapis zachowuje zgodne zastosowane maski bez pytania o ich pominięcie.
Powiązanie z tożsamością dokumentu i rewizją pozwala też na Undo/Redo;
nowsze niezastosowane zmiany nadal mają ochronę No/Yes i osobny stan zapisu.
**GUI9 PASS; 25 projektów/60 masek, UVtools18 eksportów/54 warstwy;
CTest19/19 (100,35 s), trzynaście Quit0/INET0.** Aplikacja `b39232d5…`,
silnik bez zmian, launcher47 aktualny. Odbiór dotyczy oczekującej aktualizacji.
Praca/Cancel/podmiana sesji, wydajność R01/R02 i pełny refaktor nadal otwarte.

**Poprzedni odbiór R11/R12/P0 — [zapis przed odświeżeniem metadanych](M2_MODEL_PROJECT_METADATA_PENDING_2026-11-08.md).**
Naprawiono utratę masek po zmianie ekspozycji oraz eksport ze starą ekspozycją
po restarcie. Własna kopia kompletnego wyniku i wspólny harmonogram slicingu/
zapisu zachowują PNG, Z i ręczne parametry. Usunięto zbędne kopiowanie historii.
**GUI6 PASS, UVtools12 eksportów/36 warstw, 17 projektów/39 masek;
CTest19/19 (99,62 s), osiem Quit0/INET0.** Aplikacja `2d1eb174…`, launcher47
aktualny. Zakres: gotowy wynik bez edycji i wcześniej zapisane Apply przy
oczekującej zmianie ekspozycji. Worker/Cancel, niezapisane Apply po zmianie
metadanych, wydajność R01/R02 i główny refaktor nadal otwarte.


**Poprzednia poprawka R11/R12/P0 — [zastosowane, lecz niezapisane edycje PNG](M2_MODEL_PROJECT_STALE_EDITS_2026-11-08.md).**
Naprawiono rzeczywisty błąd `Invert → Apply → przesunięcie modelu → zapis`:
3MF pomijał maski bez pytania. Stan Apply jest teraz oddzielony od rewizji
zapisanej w 3MF. Cztery warianty Apply/zapis/Undo/Redo PASS; No zachowuje
istniejący plik, Yes zachowuje osobną starszą sesję. **CTest19/19 (86,56 s),
UVtools12 plików/36 warstw, sześć Quit0/INET0; zapis/restart i R05 regresja PASS.**
Aplikacja `013e6856…`; launcher aktualny. Pozostałe stany worker/Cancel,
wydajność R01/R02 i pełny refaktor pozostają otwarte.

**Odebrane R05/P0 — [rzeczywista utrata przechwycenia myszy](M2_LAYER_CAPTURE_LOSS_2026-11-08.md).**
Cztery scenariusze pędzla/gumki/suwaka/historii oraz nowe rysowanie po
anulowaniu PASS. Zakres i Undo/Redo zachowane przed puszczeniem myszy.
**6 projektów / 18 masek; UVtools 4 eksporty / 12 warstw; Quit0 ×2, INET0.**
Wdrożono trwałe testy GUI i niezależny readback. Produkcja nie wymagała nowej
poprawki; binaria R02 pozostają identyczne, wcześniejszy CTest19/19 zachowany.
Szersze R05, wydajność R01/R02 i stany zapisu R11/R12 nadal otwarte.

**Najnowsze wdrożenie — R02/P0: [wspólny koder CXDLPv4](M2_CXDLPV4_RLE_RUNS_2026-11-07.md).**
Aplikacja `19260b2b…`, silnik `8796b10d…`. Eksport 24 gotowych warstw
4096×2560: **104,928→84,209 ms (−19,75%)**, 8 prób ABBA/BAAB; RSS +0,12%.
Wszystkie 16 plików dużej/małej/szumowej serii zgodne bajtowo. 73 791 asercji
kodeka na obu wersjach; **CTest19/19, zero pominięć**. Ramka:213 masek/452
podpory zgodne; zmienia się tylko data i checksum. GUI model/3MF/restart/
Invert/Undo/Redo/Apply/eksport PASS, dwa Quit0/INET0. UVtools łącznie11
plików/495 warstw. Dwa błędy QA wyjaśnione w raporcie; launcher43 aktualny.
To przyspieszenie eksportu rastrów, nie pomiar całego slicingu ani odbiór
całego formatu. Dalej R01 geometria/podpory, pozostałe R02, R05 capture-loss
oraz R11/R12 zapis podczas pracy/Cancel/stale. Główny refaktor pozostaje otwarty.
[Odbiór](../build/cxdlpv4-rle-runs-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).

**Poprzedni odbiór — R11/P0: [zapis masek modelu w 3MF](M2_FRESH_MODEL_PROJECT_2026-11-07.md).**
Aplikacja `a6a9f8d1…`, silnik `b52d891a…`. Gotowy 3MF zachowuje maski przed
pierwszą edycją. Po restarcie ponowny zapis wybiera aktualne zmiany Apply,
a nie starszą kopię importu. Naprawiono wykrytą rozbieżność projektu i eksportu.
**GUI restart/Invert/Undo/Redo/Apply/3MF PASS; UVtools3 pliki/9 warstw,
zero różnic; pełne ustawienia/fingerprint po restarcie zgodne. CTest19/19
(86,43 s), zero pominięć.** Dwa zwykłe Quit0/INET0; launcher42 i instrukcja
aktualne. [Odbiór zakresu](../build/fresh-model-project-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Pozostałe stany zapisu R11 (praca/Cancel/nieaktualny wynik) mają osobne karty.
**Dalej P0:** R01/R02 wydajność i R05 interakcje. Główny refaktor trwa.

**Odebrane — R10/R11/QA: [trwałe fixture'y migracji PM4n](M2_PM4N_DURABLE_MIGRATION_2026-11-07.md).**
48 projektów/96 warstw przeniesionych bez zmiany bajtów poza build;
8326 asercji PASS. **Domyślny CTest19/19 (114,62 s), zero pominięć**,
łącznie z wcześniejszymi migracjami modelu i PCB. Wspólne kontrole wydzielone
do modułu; aplikacja i silnik identyczne bajtowo. [Odbiór](../build/pm4n-legacy-ci-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).

**Poprzedni odbiór — R10/R11/R02: [PM4n, paleta i czas515](M2_PM4N_PRODUCERS_2026-11-07.md).**
Aplikacja `d3c4a587…`, silnik `b52d891a…`. Model, kalibracja i PCB używają
zgodnej palety w nowych zadaniach auto/515; kontener zapisuje czas zgodny
z harmonogramem UVtools. **106887 asercji / 3 przypadki PASS; UVtools130
plików/282 warstwy/72 projekty, zero różnic; CTest18/18 i sześć audytów PASS.**
Historia48 projektów przed i po bez zmiany masek/metadanych;42 pliki516
identyczne bajtowo. GUI: model Slice/Edit/PNG/Invert/Undo/Redo/Apply/3MF,
kalibracja No/Yes/restart i PCB merge/invert PASS. Niezależny odczyt pięciu
plików GUI/11 warstw PASS, zwykłe Quit0/INET0. Launcher42 i instrukcja aktualne.
[Odbiór zakresu](../build/pm4n-producers-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Błędy sterownika GUI (układ prywatnego ekranu i sposób zapisu modelu)
wyjaśniono; nie wdrożono spekulacyjnej zmiany produkcyjnego GUI.
Dalsze odebrane etapy: trwałe fixture'y migracji i zapis świeżych oraz
ponownie edytowanych masek w 3MF, opisane powyżej. Pozostałe wersje/parametry
PM4n i szersze stany R11 nadal wymagają wdrożeń i osobnych testów.
R01/R02 wydajność, R05 interakcje i cały główny refaktor pozostają otwarte.

**Poprzedni odbiór — R01/P0: [szybsze przygotowanie nawisów](M2_OVERHANG_SAFETY_REUSE_2026-11-07.md).**
Aplikacja `c0be4f30…`, silnik `61eb22a4…`. Slicing ramki **24,452→22,553 s
(−7,76%)**, próbkowanie nawisów **−58,03%**, RSS +0,54%; osiem prób ABBA/BAAB,
oba bloki szybsze. Lokalnie wykorzystujemy identyczny offset poprzednich
kształtów; kolejność i geometria pozostają zachowane. Limit zapamiętanych
wektorów wynosi 8 MiB na zadanie warstwy, bez obietnicy limitu całego procesu.
**21 wcześniejszych testów / 11 955 asercji na obu wersjach; 499 nowych
asercji / 3 przypadki w produkcji. CTest18/18, retained12/1140/4, sześć audytów PASS.**
16 par SL1 / 3408 PNG oraz 24 pary CXDLPv4 zgodne. UVtools: dziewięć plików /
1563 warstwy; dodatkowo pięć eksportów GUI / 640 warstw. GUI: zakres2–4,
pędzel/gumka, historia/Esc/suwak, projekt, Quit0/INET0, bez reslicingu.
[Odbiór](../build/overhang-safety-reuse-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Instrukcja i launcher aktualne. Wynik pomiarowy dotyczy izolowanego kandydata
o identycznych ciałach wdrożonych funkcji; końcowy pojedynczy przebieg produkcji
27,085 s był kontrolą zgodności, nie nowym A/B. Opisano rozrzut i błąd QA.
**Dalej P0:** R02 pozostały eksport/formaty, R01 pozostałe koszty geometrii,
R05 capture-loss i szersze GUI. Pełne cele1–15 i R01–R23 pozostają otwarte.

**Poprzedni odbiór — R05/P0: [partie PNG i bezpieczna historia pędzla](M2_LAYER_PAINT_BATCHES_2026-11-07.md).**
Aplikacja `6a181b12…`, native `42d46902…`. Edycja 128 warstw 8192×4096:
promień 3 **2,172→1,207 s (−44,42%)**, promień 64 **2,252→1,249 s (−44,52%)**.
24 próby ABBA/BAAB; wszystkie CTB identyczne bajtowo, RSS wzrósł poniżej 0,7%.
PNG są przetwarzane w ograniczonych partiach. Postęp i historia pozostają
sekwencyjne; starsza operacja nie nadpisuje zmiany, Undo ani podmiany
wykonanej przez callback. TDD: 40 nieudanych asercji bazowych → 66/66
kandydata; dodatkowo 235 asercji zachowania na obu wersjach.
**Produkcja: 301 nowych asercji / 6 przypadków PASS; CTest 18/18 (88,70 s),
retained 12/1140/4 i sześć audytów PASS.** GUI przed/po na 1600×720: po 640
masek eksportów, 128 projektu, 6 CRC; historia/Esc/suwak, Quit 0 / INET 0,
bez ponownego slicingu. UVtools: pięć CTB/390 unikalnych warstw oraz pięć
nowych eksportów GUI/640 warstw; piksele i parametry zgodne.
[Końcowy odbiór](../build/layer-paint-batches-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Dwa błędy QA zachowano i wyjaśniono w raporcie. Instrukcja i launcher aktualne.
Wynik dotyczy edycji na izolowanym kandydacie identycznym z wdrożonym źródłem;
import, podgląd, eksport i cały slicing mają osobne koszty.
**Następne P0:** R01 slicing/podpory, R02 pozostałe koszty eksportu;
R05 capture-loss i szersza macierz interaktywna nadal otwarte. Pełne cele 1–15
i R01–R23 pozostają aktywne.

**Poprzedni odbiór — R05/P0: [szybszy szeroki pędzel i gumka](M2_LAYER_BRUSH_SPANS_2026-11-07.md).**
Aplikacja `9265c3eb…`, native `a9c14722…`. Pociągnięcie o promieniu 64 na
128 warstwach 8192×4096: **10,143→2,257 s (−77,75%)**; RSS +0,065%.
Po osiem prób ABBA/BAAB dla trzech przypadków; wszystkie **24 pliki CTB
identyczne bajtowo**, promień 3 bez regresji. Dokładne odcinki wierszy
zastępują powtarzane zapisy tych samych pikseli; paleta, PNG, historia
oraz atomowe zatwierdzanie pozostają bez zmian.
**17 892 asercje / 8 przypadków na bazie i kandydacie, następnie w buildzie
produkcyjnym; CTest 18/18 (87,90 s), retained 12/1140/4, sześć audytów PASS.**
Dwie sesje rzeczywistego GUI: Brush/Eraser 64, zakres 2–4, historia i Esc,
640 masek eksportów / 128 projektu każda, Quit 0 / INET 0, bez reslicingu.
UVtools: pięć plików CTB / 390 unikalnych warstw oraz pięć eksportów GUI /
640 warstw; piksele i parametry zgodne. Launcher i instrukcja aktualne.
[Końcowy odbiór](../build/layer-brush-span-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Pomiar dotyczy operacji edycji na izolowanym kandydacie identycznym z
wdrożonym źródłem; nie obejmuje importu, odświeżenia GUI ani eksportu.
**Dalej P0:** R05 koszt PNG przy wąskim pędzlu i duże zakresy, capture-loss;
R01 slicing/podpory; R02 pozostałe koszty eksportu i formaty. Pełne R01–R23
i wszystkie 15 punktów głównego celu pozostają aktywne.

**Poprzedni odbiór — R02/P0: [pierwszy odczyt CTB](M2_CTB_BOOTSTRAP_READBACK_2026-11-07.md).**
Aplikacja `934215d5…`, native `e712f203…`. Porównanie wszystkich natywnych
masek ze źródłem połączono z pierwszym odczytem; zachowano kanoniczne PNG,
limity pamięci, pełną kontrolę parametrów i końcowego pliku. Konwersja dużego
CTBv4→v5 **5,278→4,321 s (−18,13%)**, RSS −1,30%; osiem dużych i osiem małych
prób ABBA/BAAB, wszystkie kontenery zgodne poza sprawdzonym znacznikiem czasu.
**5139 nowych asercji, 1188/216 wcześniejszych, CTest 18/18 (89,63 s),
retained 12/1140/4 i sześć audytów PASS.** Anulowanie w 420 punktach zachowuje
plik, źródło i historię. UVtools potwierdził v3/v4/v5 oraz duże pliki pomiarowe
i GUI. Dwie sesje GUI: sufiks, historia, Cancel/retry 0,376 s, Quit 0 / INET 0.
[Odbiór](../build/ctb-bootstrap-readback-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Dwa błędy konfiguracji QA zachowano w raporcie; brakujące kontrole dokończono
bez zmiany binariów i bez powtarzania udanych pomiarów/CTest.
**Dalej P0:** R01 slicing i podpory; R05 capture-loss oraz duże edycje;
R02 pozostałe koszty kodowania i formaty. To przyspieszenie konwersji gotowych
warstw, nie całego slicingu. Pełny refaktor R01–R23 pozostaje aktywny.

**Poprzedni odbiór — R05/P0: [historia podczas rysowania](M2_LAYER_STROKE_HISTORY_2026-11-07.md).**
Aplikacja `8e9dfdc0…`, silnik natywny bez zmian `aed1b678…`. Rzeczywisty GUI
potwierdził RED: Ctrl+Z nie anulował pociągnięcia przed puszczeniem myszy.
Poprawka łączy obsługę klawiatury płótna i komend menu: najpierw odrzuca
niedokończoną kreskę, następnie wykonuje Undo/Redo zatwierdzonej historii.
**6 scenariuszy Ctrl+Z/Ctrl+Y/Ctrl+Shift+Z, 3 wcześniejsze Esc i 9 kontroli
suwaka PASS; 640 masek eksportów / 128 masek projektu; UVtools 5 plików /
640 warstw. CTest 18/18 (84,43 s), retained 12/1140/4, sześć audytów PASS.**
Zakres 2–4 zachowany, późniejsze puszczenie myszy nie dopisuje kreski,
brak ponownego slicingu; zwykły Quit 0 / INET 0. Instrukcja i launcher aktualne.
[Końcowy odbiór](../build/layer-stroke-history-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
To odbiór Linux/GTK i Gray8, nie całego R05 ani wydajności slicingu/eksportu.

**R01/P0 — [wariant kolejności zadań sprawdzony, niewdrożony](M2_EXTENDED_OFFSET_SCHEDULING_2026-11-07.md).**
Osiem prób: slicing 22,547→22,457 s (−0,40%), poniżej wymaganego 3%; RSS +0,96%.
Trzy małe modele przeszły bramkę regresji. 21 kontraktów / 11 955 asercji na
każdej wersji, 16 par SL1 / 3408 PNG i 24 pary CXDLPv4 zgodne. Nie wdrożono
kandydata i nie obniżono progów. Pierwszą małą próbę przerwało błędne
oczekiwanie QA dotyczące reuse; wyjaśnienie i obie wersje zachowane.
Baza pomiarów: aplikacja `95cfa474…`, native `aed1b678…`; późniejsza poprawka
historii nie zmienia silnika. **Dalej P0:** R01 koszty przygotowania podpór
i pozostałych etapów, R02 pierwszy odczyt/pozostałe formaty, R05 capture-loss
i duże edycje. Pełne R01–R23 i wszystkie 15 punktów celu pozostają otwarte.

**Poprzedni odbiór — R02/P0: [szybszy transfer parametrów CTB](M2_CTB_PARAMETER_TRANSFER_2026-11-07.md).**
Aplikacja `95cfa474…`, native `aed1b678…`. Usunięto powtórne dekodowanie PNG/RLE
podczas transferu ustawień CTB v3/v4/v5. Liczniki pochodzą z rzeczywistych
masek porównanych ze źródłem i są związane z dokumentem oraz jego generacją.
Zwykłe zapisy edycji i końcowy odczyt pliku zachowują pełną walidację.
**Duża konwersja CTBv4→v5: 8,199→5,785 s (−29,45%)**, osiem dużych i osiem
małych prób ABBA/BAAB; RSS −0,42%, mała próba +0,155 ms. Wszystkie 16
kontenerów zgodne bajtowo poza niezależnie sprawdzoną datą zapisu.
**216 nowych asercji / 2 przypadki przed i po zmianie; 1188 asercji kontroli
CTB; 21 istniejących testów OFF/ON/default po 12989 asercji; dodatkowo
420 punktów anulowania / 4653 asercje. CTest 18/18 (97,81 s), retained
12 plików / 1140 rekordów / 4 projekty, sześć audytów PASS.** UVtools:
9 plików v3/v4/v5 (27 warstw), pliki pomiarowe i osobno GUI po 262 warstwy.
GUI: sufiks, historia, 3D↔Sliced, Cancel/retry około 0,393 s, dwa zwykłe
Quit 0 / INET 0. [Końcowy odbiór](../build/ctb-parameter-transfer-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
**Następne P0:** R01 przygotowanie podpór i pozostałe etapy slicingu;
R05 capture-loss i duże edycje; R02 pierwszy odczyt i pozostałe formaty.
To zysk konwersji, nie całego slicingu. Pełne R01–R23 i wszystkie 15 punktów
celu pozostają otwarte; GUI świeżego v3, szersze DPI i sprzęt nadal wymagają testów.

**Poprzedni odbiór — R02/P0: [natywna kontrola CTB](M2_CTB_NATIVE_READBACK_2026-11-07.md).**
Aplikacja `70ef63a6…`, native `95a46549…`. Końcowa kontrola CTB v3/v4/v5
nie odtwarza dodatkowego dokumentu PNG; porównuje rzeczywiste dane warstw,
geometrię, szyfr i parametry. Naprawiono potwierdzoną testem lukę: poprawna
składniowo zmiana maski po transferze ustawień była wcześniej akceptowana.
Dodano świeży eksport CTB v3 dla reprezentowalnych ustawień jednofazowych;
nieobsługiwane niezerowe fazy/opóźnienia są odrzucane przed zapisem.
**Konwersja CTBv4→v5: 9,785→8,190 s (−16,30%)**, osiem dużych i osiem
małych prób ABBA/BAAB; RSS −0,29%, mała próba +0,37 ms (limit 10 ms).
Wszystkie 16 plików zgodne bajtowo poza sprawdzoną datą zapisu.
**1188 nowych asercji / 5 przypadków; 21 istniejących kontraktów OFF/ON/default
po 12989 asercji; CTest 18/18 (96,64 s), retained 12/1140/4 i sześć audytów PASS.**
UVtools: dziewięć plików v3/v4/v5 (27 warstw, także świeże zadania), osobno
wejścia/wyniki pomiarów i końcowego GUI po 262 warstwy. Wszystkie piksele,
udostępnione pola i sprawdzane surowe parametry globalne zgodne. Rzeczywisty
GUI: sufiks, konwersja, Undo/Redo po 3D↔Sliced, Cancel/retry około 0,38 s,
dwa zwykłe Quit 0 i INET 0. [Końcowy odbiór](../build/ctb-native-readback-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
**Dalej:** R02 koszt pierwszego odczytu i transferu parametrów; R01 przygotowanie
podpór; R05 capture-loss i duże edycje. Dedykowany GUI świeżego v3 i sprzęt
pozostają do sprawdzenia. To zysk konwersji CTB, nie całego slicingu.
Pełne R01–R23 i wszystkie 15 punktów celu nadal są otwarte.

**Weryfikacja UI — [przewijanie edytora](M2_LAYER_EDITOR_SCROLL_LAYOUT_2026-11-07.md):**
1280×800, 1024×768 i 960×640: rzeczywiste przewinięcie odsłania dolne przyciski,
kliknięcie eksportu działa; zwykły Quit 0 / INET 0. Wcześniejszy zrzut pokazywał
stan przed przewinięciem. Nie potwierdzono usterki układu; produkcja bez zmian.
R19 pozostaje otwarte dla innych DPI, klawiatury i czytelności interfejsu.

**Diagnoza R02/P0 [zakończona](M2_CONVERSION_STAGE_PROFILE_2026-11-07.md):**
w dużej konwersji CTB dwa odczyty do dokumentu warstw zajmują 3,065 s,
porównanie masek/tabela 2,069 s, transfer parametrów 2,418 s. Początkowy
zapis kontenera około 1 ms. Wyjścia zgodne z bazą poza sprawdzoną datą;
na etapie diagnozy aplikacja była bez zmian. Wynik posłużył do wdrożenia
końcowego natywnego odczytu kontrolnego CTB, odebranego w partii opisanej wyżej.
Pierwszy odczyt i transfer parametrów pozostają kolejnymi celami R02.

**Poprzedni odbiór — 7 listopada: app `92f222ef…`, native `d7a561d9…`.**
[Szybsza konwersja gotowych warstw](M2_CONVERSION_DISPATCH_2026-11-07.md)
jest włączona domyślnie: usunięto zbędną kopię obrazową i pierwszy przebieg
statystyk; końcowa walidacja masek/parametrów i atomowa publikacja pozostają.
CTBv4→v5, 128 warstw 8192×4096, osiem dużych + osiem małych prób ABBA/BAAB:
**9,338→8,855 s (−5,18%)**, RSS −2,15%, mały przypadek bez istotnej regresji.
Wszystkie bajty zgodne poza zweryfikowaną datą zapisu. To pomiar konwersji,
nie całego slicingu; procentów nie dodajemy do poprzednich optymalizacji.
**21 kontraktów OFF/ON/default po 12989 asercji; CTest 18/18 (83,88 s),
retained 12/1140/4, sześć audytów PASS.** Rzeczywisty GUI: wybór CTBv5,
korekta rozszerzenia, edycje i Undo/Redo po 3D↔Sliced, Cancel/retry dużego
pliku (około 0,38 s), dwa zwykłe Quit 0 / INET 0. Oryginalny UVtools:
262 warstwy wejść/wyników kandydata oraz osobno 262 warstwy końcowego GUI;
wszystkie piksele, udostępnione pola i surowe globalne parametry PASS.
[Końcowy odbiór](../build/conversion-dispatch-default-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
**Dalej:** R01/P0 przygotowanie podpór; R02/P0 zapis, transfer parametrów
i kontrolny odczyt konwersji; R05/P0 capture-loss i duże edycje.
R19/P1: późniejszy test wykazał pełną dostępność dolnych przycisków po
przewinięciu w trzech rozmiarach; pozostają szersze testy DPI i klawiatury.
Pełne R01/R02/R05 oraz 15 punktów celu / R01–R23 pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `df91a51a…`, native `9bec36f2…`.**
[Preferencje eksportu per drukarka](M2_PRINTER_FORMAT_PREFERENCES_2026-11-07.md)
przeszły pełny scenariusz dwóch drukarek SL1/CXDLPv4: sufiks `part.v2`,
Cancel, odmowa nadpisania końcowej ścieżki, A→B→A oraz restart projektu.
Zachowana edycja warstwy i profile; sama zmiana formatu nie reslicuje.
Usunięto 33 linie zbędnego skanera sufiksów; bazowy test zachowania także
przechodził, więc nie jest to nowa naprawa funkcjonalna ani pomiar wydajności.
**CTest 18/18 (84,77 s), retained 12/1140/4, sześć audytów PASS**.
Końcowy UVtools: sześć plików / 18 warstw i wszystkie udostępnione pola;
bezpośrednio 12 masek SL1 +3 projektu. Dwa zwykłe Quit 0 / INET 0.
Zachowano błędy QA; poprawny CTest użyto ponownie po korekcie miejsca logu
kolektora. [Końcowy odbiór](../build/printer-format-preferences-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
**Następne P0:** R01/R02 wydajność, R05 capture-loss i duże edycje.
Pozostałe wersje/aliasy R07, pełne CRUD R08/R09, DPI i samodzielne GUI smoke
pozostają otwarte. Pełny cel 15 punktów / R01–R23 nadal aktywny.

**R01 — eksperyment zakończony, bez wdrożenia:** [projekcja krawędzi](M2_CLIPPER_PROJECTION_REVIEW_2026-11-07.md)
zachowała geometrię i maski. Dwie serie ramki: −7,22% i −9,78%, lecz mały
model z otworem przekroczył bramkę regresji: +16,52 ms przy limicie 10 ms.
Kandydat nie został wdrożony; produkcyjny silnik pozostaje bez zmian.

**Poprzedni odbiór — 7 listopada: app `cbb2b97c…`, native `9bec36f2…`.**
[Esc podczas rysowania](M2_LAYER_STROKE_ESCAPE_2026-11-07.md) naprawione:
anuluje bieżące pociągnięcie przed puszczeniem myszy, zachowuje edytor,
wcześniejsze maski, Undo/Redo i zakres. Rzeczywisty GUI RED→GREEN w trzech
stanach historii. **640 masek eksportów +128 projektu exact**, pięć plików
odczytanych przez oryginalny UVtools (640 warstw, wszystkie udostępnione
parametry). CTest **18/18, 87,85 s**, retained 12/1140/4, sześć audytów PASS;
brak ponownego slicingu, zwykły Quit 0 i INET 0.
[Końcowy odbiór](../build/layer-stroke-escape-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Ta partia nie zmienia czasu slicingu/eksportu. Pełne R01/R02/R05 i główny
cel pozostają otwarte. Następne R05: capture-loss oraz trwały smoke bez
historycznych fixture w build; dalej duże edycje, dodatkowe maski i DPI.

**Poprzedni odbiór — 7 listopada: app `5cfe8fac…`, native `9bec36f2…`.**
[Podgląd dużych zakresów](M2_LAYER_PREVIEW_CACHE_2026-11-07.md) działa domyślnie:
jeden obraz powiązany z dokumentem, generacją, zakresem i płaszczyzną;
przygotowanie w tle, Cancel/retry oraz blokada rysowania na pustym płótnie.
Osiem pomiarów części natywnej zakres→zoom→pociągnięcie na 213 maskach:
**8,165→2,704 s (−66,88%)**, RSS +0,88%, 639→213 dekodowań, osiem PNG exact.
Pierwszy podgląd nadal około 2,7 s; to nie pomiar całego GUI ani nowy zysk
slicingu/eksportu. Sześć nowych testów / 86 asercji oraz Main/SUBIMGS / 1520 PASS.
Domyślny GUI: pędzel i gumka na 2–4, suwak, Undo/Redo/Apply, 512 masek eksportów
+128 projektu; duży zakres 213×5760×3600, Cancel myszą/Esc, klik pustego płótna,
ponowienie i restart, 426 masek projektów i wszystkie 218 wpisów każdego
archiwum exact. Trzy zwykłe Quit 0 / INET 0. **CTest 18/18 (96,89 s)**,
retained 12/1140/4 i sześć audytów PASS. Oryginalny UVtools sprawdził 512
końcowych warstw GUI, piksele i wszystkie udostępnione parametry.
[Końcowy odbiór](../build/layer-preview-cache-safety-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Zachowano RED API, nieudaną próbę automatyzacji oraz rzeczywisty RED kliknięcia
pustego płótna; ten ostatni przeszedł po korekcie obsługi naciśnięcia/puszczenia.
**Dalej P0:** R01 przygotowanie podpór i inne etapy, R02 konwersje/formaty/RAM/Cancel,
R05 duże edycje, capture-loss, GUI dodatkowych masek i trwałe smoke; DPI/dostępność P1.
Pełny refaktor i zgodność wszystkich formatów/drukarek pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `f5889d51…`, native `8ecc352d…`.**
[Planowanie masek SL1 w partiach](M2_SL1_MASK_BATCHES_2026-11-07.md)
wdrożone domyślnie na SSE2. Duże PNG dekodowane w ograniczonych partiach,
liczenie Gray8 korzysta z SIMD; zachowano pełne CRC, kolejność fizycznych
obliczeń i anulowanie na wątku koordynującym. Osiem pomiarów pełnego eksportu
edytora: **1,786→0,842 s (−52,87%)**, RSS +0,17%; 1704 maski oraz metadane
zgodne poza czasem utworzenia. Procentu nie dodajemy do wcześniejszych zysków.
Pięć nowych scenariuszy / 558 asercji; 28 regresji OFF 25 234, ON/default
po 26 062 PASS. Domyślny pełny model: 213 warstw, 452 punkty podpór,
dwie pary archiwów exact. **CTest 18/18 (83,51 s)**, retained 12/1140/4
oraz sześć audytów PASS. Domyślny GUI: dziewięć kontroli zakresu, pędzel
2–4, Undo/Redo/Apply, eksport i projekt; 384 maski eksportów +128 projektu,
bez ponownego slicingu, Quit 0 / INET 0. Oryginalny UVtools: 426 warstw
pomiaru i 384 warstwy GUI z kompletem udostępnionych pól PASS.
[Końcowy odbiór](../build/sl1-plan-batches-default-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Zachowano nieudaną pierwszą próbę kolektora modelu; brakowało flagi
logowania, produkcja bez zmian, poprawiony skrypt przeszedł cały odbiór.
Model i uwagi użytkownika nienaruszone. Pozostałe architektury zachowują
poprzednią ścieżkę do czasu pomiarów. Brak nowego zysku slicingu w tej partii.
**Dalej P0:** R01 przygotowanie podpór, R05 responsywność dużych zakresów,
gumka/capture-loss/restart; R02 pozostały koszt planowania (0,501 s),
konwersje, inne rozdzielczości/formaty i opóźnienie Cancel.
Pełny refaktor, kompletność formatów oraz weryfikacja na drukarkach pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `519ee3b7…`, native `d7aa5a36…`.**
[Szybszy eksport SL1 z edytora](M2_EDITOR_SL1_EXPORT_2026-11-07.md)
wdrożony domyślnie dla SL1/SL1S, również przed Apply. Prywatna kopia zachowuje
jawnie edytowane maski i parametry; historia źródła oraz Cancel/retry bez zmian.
ABBA/BAAB na 213 maskach ramki: **4,504→1,775 s (−60,60%)**, RSS −6,71%;
osiem archiwów / 1704 maski zgodne, pomijany wyłącznie czas utworzenia.
Pięć nowych scenariuszy / 1403 asercje i 14 regresji / 19 470 PASS.
**CTest 18/18 (84,34 s)**, retained 12/1140/4 i sześć audytów PASS.
Domyślny GUI: dziewięć kontroli zakresu, pędzel 2–4, Undo/Redo, eksport,
Apply i projekt; 384 maski eksportów / 128 projektu exact, bez ponownego
slicingu, Quit 0 / INET 0. UVtools: 426 warstw pomiaru i 384 GUI, wszystkie
udostępnione pola oraz piksele zgodne.
[Końcowy odbiór](../build/editor-export-snapshot-default-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Zachowano pierwszą nieudaną próbę QA i jej wyjaśnienie. Pozostałe formaty GUI
zachowują wcześniejszą ścieżkę; brak nowej deklaracji przyspieszenia slicingu.
**Dalej P0:** przygotowanie podpór R01 (13,059 s w zachowanym przebiegu ramki),
planowanie SL1 R02 (historycznie 1,445 s z 1,775 s; nowszy odbiór powyżej), duże zakresy R05.
Pełny refaktor, zgodność wszystkich formatów oraz testy urządzeń pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `b6547320…`, native `f6b4999a…`.**
[Statystyki zwalidowanych masek](M2_VALIDATED_MASK_STATISTICS_2026-11-07.md)
wdrożone domyślnie na SSE2. W ośmiu próbach ramki walidacja i statystyki
**2,040→1,129 s (−44,64%)**, cały slicing **25,096→24,211 s (−3,53%)**;
zakresy czasów całego slicingu częściowo się pokrywają. Duże maski powtarzanego
Z: etap **0,224→0,163 s (−27,33%)**, poprawne unie i odtworzenie projektu.
Sześć nowych przypadków: OFF 8865, ON/default po 9132 asercje; istniejące
15 przypadków po 635 w trzech trybach PASS. Domyślny model 213/452 exact.
**CTest 18/18 (84,80 s)**, retained 12/1140/4 i sześć audytów PASS.
GUI: dziewięć kontroli wyboru zakresu, pędzel 2–4, Undo/Redo/Apply/zapis,
384 maski eksportów i 128 projektu exact, brak ponownego slicingu,
Quit 0 / INET 0. Oryginalny UVtools: ramka 213 + UVJ 64 + GUI SL1 384
warstwy PASS w udokumentowanych zakresach.
[Odbiór](../build/mask-statistics-default-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Pierwszy wariant dodatkowego liczenia odrzucono (+13,32%); błędy nowych
fixture i kolektora pozostają w historii. Inne architektury zachowują
wcześniejszą ścieżkę do czasu pomiarów. Brak nowego zysku eksportu w tej
partii; R01/R02/R05 oraz główny cel pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `2f113864…`, native `91271dfe…`.**
[Pionowy suwak zakresu warstw](M2_VERTICAL_LAYER_RANGE_2026-11-07.md) wdrożony:
dwa uchwyty, `Single layer`, pamięć zakresu, klawiatura i cofanie przeciągania
przez Esc. Numery są synchronizowane; nakładanie odświeża się po puszczeniu
uchwytu. Cztery nowe przypadki / 177 asercji PASS. Rzeczywisty GUI: dziewięć
kontroli wyboru i pędzel wyłącznie na warstwach 2–4, pojedyncze Undo/Redo,
Apply, zapis 3MF i eksport bez ponownego slicingu; Quit 0 / INET 0.
384 maski eksportów / 128 projektu exact; oryginalny UVtools 3 SL1 / 384
warstwy i wszystkie udostępnione pola PASS. **CTest 18/18 (92,28 s)**,
zestaw 12 plików / 1140 rekordów / 4 projekty i sześć audytów PASS.
[Odbiór](../build/layer-range-gui-qa-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Zachowano RED braku suwaka, błąd Esc i późniejszy błąd matchera QA. Pełny
R05 pozostaje otwarty: duże zakresy, dodatkowe maski, capture-loss, restart,
DPI i dostępność uchwytów. R01/R02 pozostają P0; ta partia nie deklaruje
nowego przyspieszenia slicingu ani eksportu.

**Poprzedni odbiór — 7 listopada: app `f41fd02d…`, native `91271dfe…`.**
[Pędzel przy krawędzi podglądu](M2_LAYER_CANVAS_COORDINATES_2026-11-07.md)
naprawiony: ujemny ruch myszy nie maluje już przeciwległego brzegu.
Rzeczywisty GUI RED→GREEN: 920 błędnych pikseli → 81 oczekiwanych,
wyłącznie na warstwach 2–4; pojedyncze Undo/Redo, Apply i zapis projektu.
3 nowe przypadki / 30 asercji, **CTest 18/18 (96,04 s)**, retained 12/1140/4
oraz sześć audytów PASS. GUI: 384 maski eksportów / 128 projektu exact,
Quit 0 / INET 0, bez nowego slicingu. Oryginalny UVtools: 3 SL1 / 384 warstwy
z pełnym porównaniem dostępnych pól PASS. [Odbiór](../build/layer-canvas-coordinates-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Pełny R05 pozostaje otwarty. Późniejszy odbiór pionowego suwaka opisano powyżej.

**R01 — decyzja pomiarowa:** [kandydat zwartej tablicy](M2_CLIPPER_CONTIGUOUS_REVIEW_2026-11-07.md)
odrzucony przed wdrożeniem: ABBA −5,216%, BAAB +1,392%, łącznie −1,975%,
poniżej ustalonej bramki. Geometria i eksporty zgodne; nie ma nowego
potwierdzonego przyspieszenia. R01/R02 i główny cel pozostają aktywne.

**Poprzedni odbiór — 7 listopada: app `290e63e7…`, native `91271dfe…`.**
[Zapętlenie wyrównania podpór](M2_SUPPORT_ALIGNMENT_ROLLBACK_2026-11-07.md)
naprawione i [odebrane](../build/support-alignment-rollback-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
Nawis w starej wersji nie ukończył zadania przez387s; po poprawce dwa slicing:
**0,46953/0,46962s**,112 warstw/113 punktów podpór. Transakcyjny rollback
zachowuje kontakty i ograniczenia ruchu; ograniczone próby oraz Cancel.
Nowe5 testów/42 asercje, geometria1398+47+47, macierz9 par/13CRC PASS.
Ramka użytkownika zachowała213 warstw/452 punkty. Oryginalny UVtools:
213 warstw ramki i224 nawisu PASS. **CTest18/18 (84,61s)**, retained12/1140/4,
sześć audytów PASS. GUI384 masek eksportów/128 projektu, Undo/Redo/Apply
bez ponownego slicingu, Quit0/INET0.13 478 wejść sprawdzonych.
Eksperyment zwartej tablicy był w tym momencie niewdrożony; późniejszą
pełną ocenę i odrzucenie opisano powyżej. R01/R02 nadal otwarte.

**Poprzedni odbiór — 7 listopada: app `724563d8…`, native `1fef8661…`.**
[Ograniczenie zbędnych porównań geometrii](M2_CLIPPER_INTERSECTION_ORDER_2026-11-07.md)
wdrożone i [odebrane](../build/clipper-last-swap-promotion-2026-11-07/FINAL_BATCH_ACCEPTANCE.json).
ABBA+BAAB: slicing średnio **24,486→23,274s (−4,949%)** na modelu użytkownika;
zakresy częściowo się pokrywają. 18 par archiwów exact, 8×213 warstw/452 podpory,
oryginalny UVtools213 PASS. Stały kontrakt56075 wejść: RED80028→GREEN10028
porównań; nowa rejestracja CTest. **CTest18/18 (85,53s)**, retained12/1140/4,
sześć audytów PASS. Rzeczywisty GUI:384 masek eksportów/128 projektu,
Undo/Redo/Apply bez ponownego slicingu, Quit0/INET0. Model i feedback nienaruszone.
To odbiór tej optymalizacji; pełna wydajność R01/R02 i główny cel pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `723f88a1…`, native `f2d51dae…`.**
[Diagnostyka kernela podpór](M2_PENINSULA_KERNEL_2026-11-07.md)
[odebrana](../build/peninsula-kernel-integration-2026-11-07/FINAL_BATCH_ACCEPTANCE.json):
1398 asercji exact + po47 OFF/ON, model213/452, 3 pary archiwów/4CRC,
**CTest17/17 (94,90s)**, retained12/1140/4 i sześć audytów PASS.
13 458 wejść sprawdzonych. Profil wskazuje obrys zewnętrzny75,938%,
otwory23,914% sumy nakładających się przedziałów; to nie czasy CPU/wall.
67 próbek stosów wskazuje m.in. sortowanie przecięć. Następny eksperyment
ogranicza porównania uporządkowanego końca listy; izolowane56 075 wejść
zachowuje kolejność zdarzeń. Późniejsze wdrożenie i pełny odbiór opisano powyżej.
Brak nowej deklaracji przyspieszenia slicingu. Pełny cel aktywny.

**Poprzedni odbiór — 7 listopada: app `a270533c…`, native `9c8813a3…`.**
[Czytelny komunikat CXDLPv4](M2_CXDLPV4_DIAGNOSTICS_2026-11-07.md)
[odebrany](../build/cxdlpv4-diagnostics-integration-2026-11-07/FINAL_BATCH_ACCEPTANCE.json):
RED32/14→GREEN2413/13; żądana i odczytywana wartość, jednostka, przyczyna
oraz droga do ustawienia. Rzeczywisty GUI: tekst czytelny, brak publikacji
pliku i zmian profilu, powrót do gotowych warstw bez ponownego slicingu,
Quit0/INET0. Zmieniono tylko treść odmowy i jej testy. Ostatnia pełna
regresja17/17 dotyczy poniższej partii SL1; po zmianie komunikatu wykonano
celowane13 przypadków i GUI.13 436 wejść sprawdzonych. Pełny cel aktywny.

**Poprzedni odbiór — 7 listopada: app `57c442ee…`, native `b2a26716…`.**
[Domyślna optymalizacja eksportu SL1/SL1S](M2_SL1_PNG_2026-11-07.md)
[odebrana](../build/sl1-default-integration-2026-11-07/SL1_FINAL_BATCH_ACCEPTANCE.json).
Usunięto zbędną kopię i dekodowanie PNG z prywatnego snapshotu; pełne
sprawdzanie ZIP/CRC, parametrów, Cancel i atomowa publikacja pozostają.
ABBA na modelu użytkownika: capture + eksport **3,7975→1,7868 s (−52,95%)**.
Oryginalne UVtools426 PASS; default GUI384/128 masek, Undo/Redo i Apply bez
ponownego slicingu, Quit0/INET0. Native3126/8 oraz integracja4957/15 PASS.
**CTest17/17 (84,12 s)**, retained12/1140/4 i sześć audytów PASS.
13 428 wejść sprawdzonych; model i feedback nienaruszone. Pierwsza regresja
z nieaktualnymi licznikami testów i wcześniejsze błędy QA pozostają zapisane.
To odbiór eksportu, nie zakończenie R01/R02 ani całego refaktoru.

**Poprzedni odbiór — 7 listopada: app `f084ae85…`, native `300566b4…`.**
[Domyślne ponowne użycie kodowania CXDLPv4](M2_ENCODED_MASK_REUSE_2026-11-07.md)
wdrożone i [odebrane](../build/encoded-raster-reuse-default-2026-11-07/R02_FINAL_BATCH_ACCEPTANCE.json).
W ośmiu pomiarach capture + eksport gotowych warstw skrócił się średnio
**3,8846→2,9183 s (−24,88%)**; wszystkie ON były szybsze od OFF w tym etapie.
Cały slicing ma duży rozrzut i pozostaje do optymalizacji. Domyślne użycie
potwierdzone rzeczywistym RED→GREEN: **1663 asercje / 7 przypadków PASS**.
GUI bez flagi: HIT świeżego eksportu, brak kopii w edytorze, poprawne MISS po
edycji/Apply; 384 maski eksportów i 128 projektu zgodne, bez ponownego slicingu,
Undo/Redo zachowane, Quit 0/INET 0. Oryginalny UVtools: 213 masek i dostępne
parametry PASS. **CTest 17/17 (98,42 s)**, retained 12/1140/4 i sześć audytów
PASS; 13 398 wejść bez zmian. Model i uwagi użytkownika nienaruszone.
To odbiór tej optymalizacji, nie całego R02 ani pełnego celu.

**Poprzedni odbiór — 7 listopada: app `bd9c7e78…`, native `092a390e…`.**
[Slice now i zwykłe Preview](M2_SIDEBAR_PREVIEW_2026-11-07.md) wdrożone i
[odebrane](../build/sidebar-slice-preview-2026-11-07/SIDEBAR_FINAL_BATCH_ACCEPTANCE.json).
Sidebar uruchamia dokładnie jedno zadanie; Cancel, historia i retry zachowane
(512 dokładnych porównań masek). Zwykłe Preview uruchamia potrzebny slicing,
a powrót do gotowego widoku zachowuje wynik: 128 PNG i metadane zgodne.
Oba GUI Quit 0/INET 0. CTest **17/17 (94,00 s)**, retained 12/1140/4 i sześć
audytów PASS. 13 026 wejść i 175 artefaktów sprawdzonych. Model użytkownika
oraz jego uwagi bez zmian. To zamknięcie poprawki R03/R19; pełny cel aktywny.
Następujący po tej partii odbiór R02 opisano powyżej.

**Poprzedni odbiór — 7 listopada: app `8bb4e4ef…`, native `092a390e…`.**
[Anulowanie kodowania po zmianie ekspozycji](M2_METADATA_CANCEL_2026-11-07.md)
wdrożone i [odebrane](../build/metadata-raster-cancel-2026-11-07/METADATA_FINAL_BATCH_ACCEPTANCE.json).
Rzeczywisty diagnostyczny RED→GREEN: Cancel zatrzymuje koder przed pierwszym
dekodowaniem; historia i retry zachowane, po512 porównań masek exact,
Quit0/INET0. Release: 218 asercji/9 przypadków, CTest17/17 (82,71s),
retained12/1140/4 i sześć audytów PASS.12 852 wejścia i292 artefakty sprawdzone.
To odbiór callbacku; nie jest pomiarem przyspieszenia ani zamknięciem pełnego celu.
Następujący po tej partii odbiór Sidebar→Preview opisano powyżej.

**Poprzedni odbiór — 7 listopada: app `d305abbb…`, native `768821a0…`.**
[Późne anulowanie po poprawnym zapisie pliku](../build/background-export-cancel-2026-11-06/B_FINAL_BATCH_ACCEPTANCE.json)
wdrożone i odebrane. Rzeczywiste user Cancel i zmiana modelu podczas kończenia
zapisu potwierdziły prawidłowe zatrzymanie workera, wynik publikacji oraz
unieważnienie starego rastra. GUI Release zachował edycje, Undo/Redo i retry
bez ponownego slicingu:256 porównań masek eksportu i384 masek projektów exact.
Kontrakty66/6+44/1+70/3, **CTest17/17 (83,41s)**, retained12/1140/4,
sześć audytów oraz oryginalny UVtools2SL1/256warstw PASS. Wszystkie trzy
aplikacje zakończyły się Quit0/INET0; zakres diagnostyczny i poprawkę błędu
tuple/list w weryfikatorze internal opisuje [raport R03](M2_MAIN_WORKER_CANCEL_2026-11-07.md).
12 831 wejść i454 artefakty końcowo sprawdzone. To odbiór tej poprawki;
pełny refaktor oraz wydajność R01/R02 pozostają otwarte.

**R01 — aktualny profil zakończony:** [D0/P/D1 i9 dokładnych porównań](../build/peninsula-current-profile-fixed-2026-11-07/runs/comparison-v1/result.json)
PASS:213 masek/rekordów i452 uporządkowane podpory zgodne. Kontrole D0/D1:
slicing23,877/24,398s, eksport frozen2,809/2,830s. Profilowany P:24,005s;
minimum-offset zajmuje84,13% sumy nakładających się przedziałów zadań,
z218/381 żądań poza reuse. To wskazanie kandydata, nie nowy wynik przyspieszenia.
Profil wykonano na `d305abbb…`; w tej partii zmieniono tylko fixture, native
`5191e3d6…`, po47 asercji OFF/ON.12 847 wejść, model i feedback bez zmian.

**R01 — direct-offset wstrzymany:** izolowany test obu istniejących API
wykazał 48 różnic literalnych wektorów konturów dla figury z trzema otworami
(8928 pozostałych asercji PASS, native exit42).
[Wynik i pełny XML](../build/performance-2026-11-06/slicing/next-peninsula-offset-plan-2026-11-07/exact-api-contract/probe-isolated-v3/result.json).
Kandydat nie został wdrożony; produkcja, aplikacja i aktywne testy bez zmian.
Nie normalizujemy wyników, aby ukryć różnicę.

**Teraz:** R01 — last-swap odebrany; następny krok to dalsze ograniczenie
kosztu pracy na krawędziach przy zachowaniu dokładnej kolejności przecięć,
oraz szersza macierz modeli/etapów, RAM i Cancel. Stały test56075 wejść
jest obowiązkową kontrolą każdej kolejnej zmiany algorytmu.
Szerszy cache całych wejść nadal ma limit korzyści0,1722% na badanym modelu. R02 — domyślne
usprawnienia SL1/SL1S i CXDLPv4, bezpośredni eksport edytora oraz partie
planowania odebrane. Dalej inne modele/formaty, pozostały koszt i latencja Cancel.
R19: komunikat CXDLPv4 odebrany; dalej strukturalne błędy walidacji,
przejście do ustawienia, nagłówek modalu i lokalizacja. Pełne piony R01–R23 pozostają w planie.

**Odebrane R03/R19 — podwójny start po „Slice now”:** druga próba metadata
potwierdziła stosem GUI wywołania Sidebar→reslice→Preview→reslice→set_task→
stop_internal. Preview anuluje właśnie uruchomione zadanie, a GUI czeka na
workera. [Rzeczywisty stos](../build/metadata-raster-cancel-2026-11-07/adapter-v1/runtime-v3/runs/metadata-old-v2/controller/unexpected-cancel-gui-stack.json).
Poprawka routingu v2 oraz oba scenariusze GUI i końcowa regresja są odebrane.
Obie wcześniejsze próby nadal mają status
FAIL QA względem planowanego user-Cancel; nie doszły do tego kliknięcia.
Test brakującego callbacku użył zwykłego menu Ctrl+R i potwierdził RED,
zachowując ścisłe sprawdzenia Cancel, historii i ponowienia.

**Poprzedni odbiór — 7 listopada: app `a3662182…`, native `768821a0…`.**
[Główny eksport SLA: Cancel, zachowanie warstw i retry](../build/background-export-cancel-2026-11-06/A_FINAL_BATCH_ACCEPTANCE.json)
ma dwa rzeczywiste RED→GREEN, kontrakty135/8, pełny GUI z powrotem0,432s,
256 porównań masek eksportu i384 masek trzech projektów, zwykłe Quit0/INET0.
**CTest17/17**, retained12/1140/4, sześć audytów oraz oryginalny UVtools
2SL1/256warstw PASS; 13 221 wejść pozostało zgodnych. To odbiór A0+A+A1;
późne Cancel po publikacji B oraz cały refaktor pozostają otwarte.

**Poprzedni odbiór — 7 listopada: app `58711552…`, testy `7cec194c…`.**
[Domyślna optymalizacja połączeń podpór i odpięcie libvgcode](../build/support-links-promotion-2026-11-06/FINAL_BATCH_ACCEPTANCE.json):
build, rzeczywisty default213/452, trzy dokładne porównania z CRC,
**CTest16/16**, retained i sześć audytów PASS. Widoczny model w3D i Preview,
zwykłe Quit0/INET0; zakres wizualnego odbioru jest osobno zapisany.
Kontrolowana partia kandydata skróciła etap linków średnio o55,30%; nie dowodzi
stabilnego skrócenia całego slicingu/eksportu. W finalnym pojedynczym odbiorze
linki1,581s. R01/R02 oraz pełny cel pozostają otwarte.

**Poprzedni odbiór: app `26bedf70…`, testy `a2046f95…`.**
[Szybki podgląd SL1/SL1S i naprawa listy formatu](M2_SL1_TRANSITION_PREVIEW_2026-11-06.md):
1473/4 nowych kontraktów PASS. Na własnych128 dużych maskach jeden preflight
0,681 s → 0,000137 s; to oszczędność tego etapu, nie całego eksportu.
Oryginalny UVtools:92 pliki/276 warstw/64 projekty PASS.
Rzeczywisty GUI SL1S ujawnił blokowanie zapisu formatu przez handler opisu;
po dodaniu Skip ten sam test przeszedł bez zmiany oczekiwań. Końcowe GUI
SL1 i SL1S: Cancel, historia, zapis preferencji SL1S i retry128 exact,
dwa Quit0/INET0. CTest14/14 (96,73 s), retained12/1140/4 i pięć audytów PASS;
8578 wejść bez zmian. Pełny cel nadal ACTIVE.

**Historia odebranego R03:** [główny worker](M2_MAIN_WORKER_CANCEL_2026-11-07.md): najpierw dostępność Cancel w głównym eksporcie, potem
zachowanie gotowych warstw i osobno późny Cancel po publikacji. Root znalazł
konkretną lukę: `export_gcode()` ustawia `show_warning_dialog=true`, co pomija
`on_slicing_began()` i jedyne pokazanie przycisku Cancel.
[Rzeczywisty RED braku Cancel](../build/background-export-cancel-2026-11-06/A0_RED_ACCEPTANCE.json)
jest potwierdzony: kompletny staging ZIP128, właściwy przycisk ukryty i
nieaktywny, stary plik zachowany podczas obserwacji. Eksport zakończył się
naturalnie bez wysyłania Cancel; Quit0/INET0 i piny PASS. A0 po buildzie
`f27fd796…` rzeczywiście przywróciło widoczny, aktywny Cancel; anulowanie
zachowało stary plik i usunęło staging. Ten sam test dał osobny
[RED utraty gotowych akcji](../build/background-export-cancel-2026-11-06/A_READY_UI_RED_ACCEPTANCE.json).
**A+A1 są wdrożone i zbudowane: app `a3662182…`, native `768821a0…`.**
Kontrakty wx21/4, backend44/1 i postęp70/3 PASS; nowe niepokryte zależności0.
[Pełny GUI A przeszedł](../build/background-export-cancel-2026-11-06/A_GUI_ACCEPTANCE.json):
Cancel0,432s, stare dane i Undo/Redo zachowane, retry bez rasterowania,
256 porównań masek eksportu i384 masek trzech projektów exact; Quit0/INET0.
Końcowa regresja17 i oryginalny UVtools przeszły na tej samej aplikacji.
**Historia R03 B — wynik eksportu po późnym Cancel (odbiór zakończony powyżej).**
[Rzeczywisty RED](../build/background-export-cancel-2026-11-06/B_LATE_PUBLICATION_RED_ACCEPTANCE.json)
potwierdził błąd na diagnostycznym buildzie tych samych źródeł A: po publikacji
pełnego SL1 ze 128 warstwami zdarzenie i GUI przyjęły `Cancelled` zamiast
`Finished`. Worker prawidłowo przeszedł przez CANCELED→IDLE. Trzy pliki
natywne i trzy projekty zachowały wszystkie maski/parametry, Undo/Redo i retry;
zwykłe Quit0, INET0, niezmienione wejścia i końcowy exit42. Ten wynik dotyczy
semantyki na diagnostycznym buildzie, bez wniosku o wydajności Release.

**B+B1+B2 zbudowane: app `d305abbb…`, native `768821a0…` bez zmian.**
[Build](../build/background-export-cancel-2026-11-06/v3/b-integration-v3/runs/build-b-v3/result.json)
i [kontrakty](../build/background-export-cancel-2026-11-06/v3/b-integration-v3/runs/contracts-b-v1/result.json)
przeszły: wx66/6, eksport44/1, postęp70/3, bez failures/skips. 11 273 wejścia
zgodne; nowych niepokrytych zależności0. Pierwszy build wykrył dostęp do
chronionego mutexu w nowym teście; B2 poprawiło wyłącznie ten test, zachowując
asercje. Druga próba zatrzymała się przed configure na błędnym opisie receipt
w narzędziu QA; trzecia użyła sprawdzonych rzeczywistych danych i przeszła.
Poprawka produkcji zachowuje stan anulowania potrzebny do zatrzymania workera,
a skuteczną publikację rejestruje osobno. Diagnostyczny build trzech jednostek
oraz rzeczywiste scenariusze późnego user Cancel i internal invalidation już
przeszły: CANCELED→IDLE, poprawny wynik publikacji, Quit0/INET0. User zachował
historię i retry bez rasterowania; zmiana X o1mm unieważniła stare warstwy,
wykonała nowy raster i zachowała poprawny nowy eksport. Niezależne porównania
obejmują odpowiednio3/4 stosy natywne i3/1 stosy masek projektu po128warstw.
Weryfikator internal miał błąd tuple/list; poprawiono wyłącznie typ oczekiwania
i ponownie sprawdzono te same artefakty. Pierwotny wrapper1 pozostaje zachowany;
nie jest przypisywany jako sukces całego wrappera.
**Końcowy odbiór B zakończony:** GUI Release, regresja17 i oryginalny UVtools
PASS; bieżący następny krok to R01/R02, jak w podsumowaniu powyżej.
R02: przygotowane ponowne użycie zakodowanych warstw ma testy rzeczywistego
HIT/MISS w writerze i anulowania końcowych statystyk. [Dodatkowy przegląd](../build/encoded-raster-reuse-2026-11-06/v3/ROOT_FOLLOWUP_REVIEW.md)
wskazał dwie kopie RLE i koszt capture edytora; trzeba zmierzyć całość przed
włączeniem optymalizacji. Wykrył również istniejący brak callbacku Cancel
podczas ponownego kodowania po zmianie metadanych. [Osobna poprawka tej ścieżki](../build/metadata-raster-cancel-2026-11-07/TEST_DESIGN_PL.md)
ma priorytet P1 i nie będzie łączona z odbiorem cache.
R01: [następny plan przygotowania podpór](../build/performance-2026-11-06/slicing/next-peninsula-offset-plan-2026-11-07/PLAN_PL.md)
oddziela aktualne koszty peninsulas6,18–6,43s / extended shapes4,53–4,54s od
historycznej wskazówki o zbędnym union. Aktualny profil D0/P/D1 jest już zakończony; przygotowano test literalnej
zgodności API i niewdrożony kandydat direct-offset. Nie ma jeszcze wyniku
wydajności tej propozycji.

[Odbiór połączeń podpór](M2_SUPPORT_LINK_PROFILE_2026-11-06.md) zachowuje całą
serię ABBA, 541 przypadków OFF/ON, niezależny UVtools i jawny wzrost frozen
export+10,74% z dodatkową kontrolą bez podpór. Dwa błędy kolektora zależności
przed kompilacją zachowano jako błędy QA; trzeci build i finalna regresja PASS.

**Poprzednia odebrana partia: app `f168bf54…`, testy `518b4d6…`.**
[Końcowy odbiór partii](../build/default-bbox-sl1-cancel-2026-11-06/FINAL_BATCH_ACCEPTANCE.json):
CTest **14/14**, retained 12 plików / 1140 rekordów / 4 projekty i pięć audytów
PASS. 7970 wejść końcowo zgodnych. Bbox jest już **domyślnie włączony**;
rzeczywisty default dał 44,959 s slicingu, 213 dokładnych masek i 452 podpory.
To jedna próba odbioru, a kontrolowany wcześniejszy ABBA dał średnio −17,45%.
Niezależny odczyt bieżących 92 SL1/SL1S i 213-warstwowego CXDLPv4 PASS.

Rzeczywisty GUI SL1: Cancel przy 26/128 masek 4096×4096, powrót po 0,953 s,
stary plik, dirty i Undo/Redo zachowane. Retry i zapis projektu zachowały
wszystkie 128 masek i parametry; zwykłe Quit 0, INET 0.
[Zakres i ograniczenia R03](M2_SL1_PLAN_CANCELLATION_2026-11-06.md).

**Poprzednia odebrana partia: app `366cf454…`, testy `80a18783…`.**
[R02/R10 — paleta i kontury](M2_PALETTE_AND_CXDLPV4_BOUNDS_2026-11-06.md):
walidacja publiczna 213 masek średnio 3,048→1,954 s (−35,91% w tym etapie);
wszystkie PNG, metadane i callbacki identyczne, późny OLD 3,060 s.
Publiczne testy palety 368/1 PASS przed i po zmianie. Błąd odmowy większych
rastrów CXDLPv4: rzeczywisty RED 8/94 → GREEN 230/2. Niezależny UVtools
potwierdził 24 pliki/warstwy i 1802 sprawdzenia, w tym edycję. Osobny pomiar
samego zapisu nie wykazał stabilnego przyspieszenia (nakładające się zakresy).
[Odbiór końcowy](../build/palette-contour-integration-2026-11-06/FINAL_BATCH_ACCEPTANCE.json):
CTest **14/14 (124,00 s)**, retained 12/1140/4 i pięć audytów PASS; 8548
wejść bez zmian. To odbiór tej partii, bez zamknięcia całego R02/R10.

**Poprzedni odbiór R03: app `a082bc90…`, testy `e8a58bd8…`.**
[Końcowy eksport edytora](M2_EDITOR_FINAL_EXPORT_CANCEL_2026-11-06.md):
16 344 asercje / 6 przypadków SL1/SL1S PASS, rzeczywisty GUI SL1 Cancel po
kompletnym ZIP128, powrót 0,789 s, stary plik i historia zachowane.
Retry/projekt128 dokładnych masek i parametrów, Quit0/INET0.
CTest14/14 (96,43 s), retained12/1140/4 oraz pięć audytów PASS; 8559 wejść
bez zmian. To odbiór tej ścieżki, nie całego R03.

Pozostałe ścieżki głównego workera, natywne dialogi z zarządcą okien
i pełna macierz formatów nadal mają osobne otwarte bramki.

Poniższe pomiary bazowe i wcześniejsze odbiory zachowują wskazane wersje;
nie opisują wszystkich własności ostatniego builda.

**Pełny cel pozostaje ACTIVE. Najwyższy priorytet użytkownika: slicing na wielu
etapach i eksport nadal trwają zbyt długo.** Nowy baseline na niezmienionej
produkcji: slicing **57,84–69,37 s** (213 warstw / 452 punkty podpór), osobny
eksport gotowych warstw CXDLPv4 **8,35–9,48 s**, mediana 8,45 s. Intel Core
i5-14450HX, 16 logicznych CPU. To pomiary przed poprawką, bez deklaracji zysku;
szczegóły i źródła są w [planie](PLAN_WDROZEN_REFAKTORU_2026-11-06.md).
Historyczne −4,104% całego slicingu nie zamyka zgłoszenia użytkownika.

Test postępu odtworzył rzeczywistą usterkę: **2/8 asercji FAILED** — 100%
pojawiało się już podczas rasteryzacji, przed końcowym zakończeniem. Poprawka
po pełnym buildzie przeszła **70 asercji / 3 przypadki**, obejmujące anulowanie
i zachowanie edycji. Kontrole geometrii 23/1 oraz focused 5509/39 również PASS.
[Raport RED](../build/performance-2026-11-06/export/test-baseline/CONTRACT_RESULTS.json)
pozostaje zachowany. Eksperyment clippingu zachował dokładną geometrię, ale
ujawnił nadmiarowe checkpointy; po jednogliniowej korekcie produkcji
ponowny build i wszystkie siedem kontraktów PASS, w tym ON/OFF po 1354/3.
Testów nie osłabiono; w tamtej partii eksperyment był domyślnie OFF.
[Historyczne kontrakty app d8007176…/test 901df345…](../build/performance-2026-11-06/export/candidate-contracts-v2/CONTRACT_RESULTS.json).

Nowa [regresja na zestawie 248 wejść](../build/performance-integration-2026-11-06/REGRESSION_RESULTS.json)
zakończona: **CTest 13/13 (159,64 s)**, retained 12 plików / 1140 rekordów /
4 projekty oraz pięć audytów — wszystkie siedem bramek z kodem 0.
[Rzeczywisty startup GUI aplikacji d8007176…](../build/performance-integration-2026-11-06/GUI_STARTUP_EVIDENCE.json)
przeszedł partial-right [368,100,912,588], zwykłe Quit 0 i INET 0;
root obejrzał zrzut okna. To jeden scenariusz startup na nowym binarium;
wcześniejsze cztery scenariusze zachowują własny odbiór.

[Odebrany fragment R02](M2_EXPORT_PERFORMANCE_2026-11-06.md): eksport CXDLPv4
213 warstw średnio **7,826 s**, bezpośrednia późna kontrola starego binarium
**9,455 s** — zaobserwowane **17,23% mniej**. Trzy próby na wersję; starsza
baza 5,091 s pokazuje zmienność hosta i pozostaje w raporcie. Wszystkie
9 plików zgodne poza datą/CRC. Niezależne pola i piksele PASS dla
92 SL1/SL1S (276 warstw, 64 projekty), 6 własnych masek CXDLPv4 i ramki
213 warstw. Brak nowego pomiaru SL1; pełny pion wydajności nadal otwarty.

Rekompilacja starej produkcji nowym toolchainem dała inną bazę: eksport
**4,18–5,76 s**, slicing w jednej próbie **51,71 s**. Nie jest to wynik
optymalizacji kodu. Porównania używają zgodnego toolchainu i osobnej
kontroli, patrz [bieżąca integracja](PLAN_WDROZEN_REFAKTORU_2026-11-06.md#bieżąca-integracja-zmian).

Odczytano zakończoną [regresję aplikacji 68fc25fd…](../build/offscreen-window-integration-2026-10-16/REGRESSION_RESULTS.json):
**CTest 11/11**, retained 12 plików / 1140 rekordów / 4 projekty i audyty
preview, session, FFF, TBB oraz obiektu viewer zakończyły się kodem 0.
To wyniki już wykonanego przebiegu z 16 października, a nie nowy benchmark.
Poprawka położenia okna ma build PASS oraz 1671 asercji / 4 przypadki;
[rzeczywiste GUI czterech scenariuszy jest odebrane](../build/offscreen-window-integration-2026-10-16/FINAL_WINDOW_GEOMETRY_ACCEPTANCE.json):
control [0,0,1280,800], partial-right [368,100,912,588], bottom
[100,212,912,588] i oversize [0,0,1280,800]. Cztery zwykłe Quit 0, INET 0
oraz niezmienione 239 wejść. To zakres nonmaximized na ekranie 1280×800;
DPI, pozostałe monitory i maximize/restore pozostają osobnymi wymaganiami.
Wcześniejsze pakiety odbioru zachowują własny zakres.

Publiczny plan SL1/SL1S ma osobny [odbiór R03](M2_SL1_PLAN_CANCELLATION_2026-11-06.md):
rzeczywisty **RED 18/186 → GREEN 540**, build 0 i dotychczasowe export/statistics 5509/39
PASS. Checkpointy zachowują geometrię, materiał i źródło; niezależne
92 pliki SL1/SL1S (276 warstw, 64 projekty) oraz domyślny CXDLPv4 213/213 PASS.
Wszystkie 8290 wejść tego odczytu bez zmian. Runtime Cancel edytora SL1
odebrano później na 128 dużych maskach (opis powyżej). Końcowy zapis edytora i GUI SL1S odebrano później
w partii opisanej na początku dokumentu. Główny worker Cancel/stale-generation
nadal pozostaje **pending**. Własny duży fixture GUI porównano
Python/Pillow/raw INI; 92 pozostałe pliki odczytano także oryginalną referencją.

| Priorytet / pion architektury | Stan i właściciel | Następny dowód / zależność |
|---|---|---|
| P0 — przygotowanie modelu, podpory, slicing | PARTIAL — bbox wysp i połączeń podpór domyślnie ON odebrane | Połączenia podpór −55,30% w kontrolowanej partii; finalny default213/452, CTest16 i exact PASS. Dalej aktualny profil peninsulas, lokalne pominięcie zbędnej operacji wyłącznie po literalnej zgodności; cały pipeline pozostaje otwarty. |
| P0 — rasteryzacja i eksport | PARTIAL — wcześniejsze optymalizacje oraz lekki preflight SL1/SL1S odebrane | Preflight128 dużych masek 0,681 s→0,000137 s; dokładne wyniki i oba GUI PASS. Dalej całość eksportu, inne modele/edycje, RAM i kontrolowane reuse/cache. |
| P0 — anulowanie i publikacja eksportu | PARTIAL — edytor SL1/SL1S i główny eksport SL1 Cancel/historia/retry odebrane | Główny worker: actual RED→GREEN,135/8, GUI0,432s, oracle256 i CTest17 PASS. Dalej późne Cancel po publikacji B, internal invalidation i szersze rzeczywiste przeploty UI/statystyk. |
| P1 — natywne dialogi Open/Save | TODO — R19/R04/R07 | Open w prywatnym Xwayland bez WM miał [38,-51,1203,902] na1280×800. Zweryfikować przyczynę, zwykły WM/DPI i dostępność przycisków; odbiór głównego okna tego nie obejmuje. |
| P0 — odzyskanie widocznego okna | DONE w zakresie czterech scenariuszy nonmaximized | Build, 1671 asercji i rzeczywiste GUI control/right/bottom/oversize PASS. Pełne DPI/monitory/maximize pozostają otwarte. |
| P1 — trwałe projekty model/PCB w domyślnym CI | DONE w zakresie dwóch trwałych bramek | Model 1404 i PCB 5840 asercji PASS, pierwsze CTest 0,42/0,54 s; dodano smoke, konfiguracja i powtórzenie 0,39/0,66 s PASS. Obie rundy: 84 piny bez zmian i pełny cleanup. [Raport](M2_DURABLE_MIGRATION_CTEST_2026-11-06.md). |
| P1 — preferencje formatu per drukarka i nazwa pliku | TODO — po bieżącej integracji | Istniejąca implementacja wymaga pełnego odbioru GUI: zmiana filtra/sufiksu, anulowanie, overwrite, A→B→A i restart. |
| P1 — edycja warstw / zakres / UI | IN PROGRESS w szerszym celu | Zachować wcześniejszy odbiór pędzla i zakresu; domknąć odkrywalność narzędzi, pełną macierz źródeł/palet, eksport oraz zapis/odtworzenie edycji. |
| P1 — modele, PCB i kalibracje wszystkich formatów | IN PROGRESS w szerszym celu | PMX2/PWMB515 mają odebrane zakresy; PM4n i pozostałe rodziny wymagają osobnych rzeczywistych testów producenta, wersji, czasu i niezależnego odczytu. Nie rozszerzać wspólnej allowlisty na niezweryfikowane źródła. |
| P1 — profile drukarki/materiału/procesu | TODO dla pełnego CRUD | Save As, edycja, rename, delete, cancel, konflikt nazw i restart z rzeczywistym użyciem wartości w slicingu/eksporcie. |
| P1 — odcięcie FFF i samodzielna dystrybucja | PARTIAL — wrapper/target/link libvgcode odpięte i odebrane w SLA | Pozostałe zależności FFF, pełna paczka offline, relokacja i licencje nadal otwarte. Renderer3D/Preview widoczny; nie oznacza to wszystkich platform i interakcji. |

Kompilacje, duże testy i sesje GUI wykonywane są kolejno, aby nie zakłócać
pomiarów i nie mieszać źródeł z innym binarium. Lekki przegląd i przygotowanie
rozłącznych zmian odbywają się równolegle. Testy funkcjonalne przechodzą przed
pomiarami porównawczymi; pogorszenie dokładności, anulowania lub zapisu blokuje
przyjęcie optymalizacji. Model użytkownika w Downloads pozostaje tylko do odczytu.

Poniższy zapis z 16 października to zachowana historia poprzedniego frontu;
ówczesne słowa „aktualny”, „trwa” i „następne” odnoszą się do tamtego momentu.
Nie zastępują powyższej kolejności i bieżących wyników.

## Historyczny front — 16 października 2026

**Ostatni odebrany złożony etap: DONE w swoim zakresie. Pełny cel nadal ACTIVE.**
Aplikacja `9c5688b2…`, testy `175d75f7…` mają [końcowy seal 144 wejść](../build/composite-sla-ui-model-2026-10-16/FINAL_COMPOSITE_ACCEPTANCE.json):
142 zweryfikowane kopie, 2 wejścia chronione tylko hashem i niezmienione
źródła/binary. All-target build PASS, CTest 10/10 (78,61 s), retained
12 plików / 1140 rekordów / 4 projekty oraz bieżące audyty PASS.

Odebrano źródło/edycję modelu 515, header cut z jawnym include Preview,
lokalne hinty, usunięcie nieaktywnego wiersza licznika i naprawę polskiej
bazy plural. Sześć sesji GUI zakończyło się zwykłym Quit 0 i INET 0:
zmiany presetów A→B→A, transakcje modelu/projektu (35 asercji, 5 zapisów,
6 odtworzonych masek dokumentu z rzeczywistą edycją warstwy) oraz osobne
wizualne przeglądy sześciu hintów EN i sześciu PL. Root zweryfikował 429
artefaktów GUI. Dwa wcześniejsze całe raporty FAILED pozostają zachowane
jako błędy QA; nie przemianowano ich na PASS.

Nowszy etap lokalizacji i trwałych fixture ma osobny [seal 159 wejść](../build/polish-sla-captions-2026-10-16/FINAL_CAPTION_ACCEPTANCE.json);
aplikacja 9c i testy 175d pozostały niezmienione. Osiem historycznych
fixture jest już przenośnych i odebranych. Poprawiono sześć polskich hintów
zgodnie z podpisami GUI; kontrakt treści 9/9 i 8568 porównań plural PASS.
Root zakończył wizualny przegląd sześciu nowych polskich kart, bez rozszerzenia starego seal 144. Wizualny odbiór
nie jest pełnym OCR ani dowodem braku niewidocznych obszarów kliknięcia.
Nie odebrano automatycznego offscreen/DPI, całej macierzy formatów/drukarek,
pełnego CRUD profili, wszystkich UVtools ani sprzętu. Ten etap nie ma
nowego benchmarku slicingu/eksportu. Wcześniejsze [czas 515](M2_ANYCUBIC_515_TIME_2026-10-16.md),
[obiekt viewer](M2_SLA_VIEWER_OBJECT_2026-10-16.md) i [legenda/owner](M2_SLA_PREVIEW_OWNER_2026-10-16.md)
zachowują własne źródła i zakresy.

**PCB 515: DONE w odebranym zakresie, pełny cel nadal ACTIVE.**
Dwa zatwierdzone wywołania producenta przeszły native GREEN 65390 asercji
oraz stary baseline 5840. Oryginalny FullOpen i niezależny odczyt nowych
144 plików / 216 rekordów / 96 projektów bez błędów. All-target build
zakończył się kodem 0: app `47d332b9…`, testy `86890d8b…`, 3781 wejść
kompilacji/zasobów niezmienionych przed/po. Root zamroził [164 wejścia](../build/pcb515-integration-2026-10-16/identity.json):
CTest 10/10 (79,51 s), retained 12 plików / 1140 rekordów / 4 projekty i
audyty preview/session/FFF/TBB/obiektu viewer PASS, wejścia bez zmian.
Rzeczywiste GUI generate→restart oraz niezależny odczyt dwóch plików /
dwóch rekordów bez błędów, dwa zwykłe Exit 0 i INET 0. [Końcowy seal PCB 515](../build/pcb515-integration-2026-10-16/FINAL_PCB515_ACCEPTANCE.json)
zamyka zakres 164. Pierwszy cały raport FAILED przy Undo bez obserwowalnej
gotowości zachowano; osobny v2b po SaveReady przeszedł. Nie udowodniono
dokładnej przyczyny pierwszej porażki. Poprzednie odbiory zachowują własne
źródła i zakresy, bez certyfikacji wszystkich drukarek/sprzętu.

**Aktualny P0: odzyskiwanie częściowo niewidocznego okna.** Rzeczywisty
RED na 47d: zapisane [1200,100,912,588] na workarea 1280×800 kończy jako
[1024,100,912,588], z 656 pikselami poza prawą krawędzią. Kontrola poprawna.
Zastosowano małą politykę rozmiaru/minimum/pozycji i samodzielne testy STD.
[All-target build](../build/offscreen-window-ux-2026-10-16/runtime/build-green/result.json)
zakończył się kodem 0 (368,66 s), przy niezmienionych 3785 wejściach źródeł/zasobów
oraz 67 plikach QA/danych. Bieżąca aplikacja to `68fc25fd…`; testy SLA pozostają
byte-identyczne `86890d8b…`. [Nowy test geometrii](../build/offscreen-window-ux-2026-10-16/runtime/unit-green/result.json)
przeszedł 1671 asercji w 4 przypadkach; rzeczywisty link zawiera tylko Catch2,
bez GUI, rdzenia slicera lub TBB. Root zamroził [239 bieżących wejść](../build/offscreen-window-integration-2026-10-16/identity.json);
szersza regresja trwa, a rzeczywiste GUI poprawki pozostaje do wykonania. Oczekiwana pozycja to [368,100,912,588], bez korekt QA.
Nie jest to jeszcze odbiór GUI, DPI, maximized ani całego multi-monitor.

| Pion funkcjonalny | Aktualny wynik | Co pozostaje lub blokuje następny krok |
|---|---|---|
| Sesja i podgląd SLA | Usunięty rzeczywisty owner GCodeViewer i jego API; zero symboli klasy w ELF. Legenda, L/menu, clipping i widoki PASS. | Kompilacja obiektu odebrana w historycznym pakiecie 130 z app/test identycznymi f6d0/02c. Nowszy header cut i jawny include Preview odebrane na 9c w seal 144. Wrapper/target nadal staged; dawne 51 lokalnych tabel to historyczny odczyt, nie aktualny pomiar ani wywołania renderera. |
| Wspólny renderer i manipulacje | Rzeczywiste Move/Rotate/Scale, Undo→zapis→Redo, Arrange, Slice i 12 masek; Measure oraz legacy transakcje PASS. | Wiele obiektów, pozostałe osie, DPI/platformy, pełna macierz payload/veto i dokładność pomiarowa narzędzi nadal otwarte. |
| Anycubic 515: model i edycja | Core GREEN: 43584 asercje; stare projekty 1404 przed i 1404 po zmianie. Oryginalny FullOpen 60 plików oraz niezależny odczyt 60 / 180 rekordów / 24 projekty bez błędów; migracja 24 odczyty / 72 rekordy bez błędów. | Trwały pakiet 12 payloadów (46388 bajtów), portable reader i osiem oczekiwanych odmów odebrane osobno: native 1404, abs/portable 24/72 bez błędów. Wcześniejsze RED zachowane. Połączona aplikacja 9c i wybrane GUI odebrane w seal 144; nie jest to certyfikacja wszystkich drukarek lub sprzętu. |
| PCB: baseline i historia | Historyczny first-PNG/history ma seal 107. Nowy rzeczywisty RED producenta 515: 4120/65390; niezależnie 192 błędy źródła/schema/kanoniczności w 144 plikach / 216 rekordach / 96 projektach. Natywne/publiczne piksele, geometria, parametry i czas poprawne; modern bez błędów. | Baseline starych 32 projektów przeszedł 5840 asercji; 64 wejścia niezmienione. Dwa wywołania producenta: native GREEN 65390/5840 i niezależny odczyt 144/216/96 bez błędów. Nowa app 47d ma osobny seal 164 z bieżącą regresją, GUI generate/restart i niezależnym odczytem dwóch plików. Szerszy moduł PCB pozostaje otwarty. Nie jest to wada natywnego kodera. |
| Menu Expert i SVG | Osobny [seal GUI 46e, 106 wejść](../build/sla-menu-ux-2026-10-16/runtime/FINAL_GUI_ACCEPTANCE.json): instancje oraz wejście Edit SVG→Escape→zapis bez zmian. | Liczbowa edycja SVG, restart i slicing zmienionego SVG nie były odebrane. Offline Expert pozostaje polityką; martwe wybory ukryto. |
| Wydajność podpór | Osobny [seal 8f, 103 wejścia](../build/peninsula-minimum-reuse-2026-10-16/runtime/FINAL_PERFORMANCE_ACCEPTANCE.json): średnio −4,104% slicing i −23,774% faza półwyspów w czterech próbach. | Jeden model/host, średni MaxRSS +39,928 MiB; brak uniwersalnej gwarancji i nowego benchmarku na 0cae. |
| Drukarki, materiały, presety i pełne UVtools | Odebrane wybrane piony i formaty mają własne raporty. | Pełny CRUD profili, cała macierz parametrów/formatów/drukarek oraz brakujące operacje i GUI nadal wymagają pracy. |
| P1 hinty offline i P2 licznik | Zastosowano pięć uzgodnionych ścieżek: lokalny katalog sześciu hintów i brak nieaktywnego wiersza licznika. Kontrakt treści i kontrole źródłowe GREEN. | Rzeczywiste GUI EN/PL i A→B→A odebrane; sześć polskich treści zgodnych z podpisami kontrolek odebrano w nowym seal 159. Pełna lokalizacja własnego UI pozostaje otwarta. Istniejący browser guard blokował URL już wcześniej; brak potwierdzonej dziury sieciowej. |
| Polska lokalizacja | Osobna naprawa zastanej bazy: strict msgfmt i pełne MO poprawne; usunięto 51 nieosiągalnych slotów, zachowano 132 osiągalne tłumaczenia, 8610 lookupów zgodnych; dodano tylko sześć hintów. | Korekta sześciu msgstr: 9 kontroli treści i 8568 lookupów PASS oraz rzeczywiste pełne karty PL odebrane. Pozostałe własne UI wymaga dalszej lokalizacji. |
| Pozycja i rozmiar okna | Potwierdzony native RED częściowo offscreen przy wymuszonym minimum. Nowy helper std-only z jawnym outer minimum i test target tylko Catch gotowe. | Build, kontrakt jednostkowy i powtórzenie oryginalnego GUI/control wymagane. Przy minimum większym od ekranu tylko zakotwiczenie; nie obiecywać niemożliwego fit. |
| Release i utrzymanie | Przebiegi odebranych GUI bez INET; trwałe testy/dokumenty widoczne po poprawce .gitignore. | Pełna paczka offline, relokacja/licencje/platformy/hardware otwarte. Ostrzeżenia wx i bookkeeping pozostają osobnym cleanup. |

### Kolejność najbliższych pionów

1. **P0 — rzeczywisty offscreen:** zbudować nową politykę i samodzielny
   kontrakt geometrii, następnie powtórzyć oryginalną kontrolę i native RED
   bez korygowania pozycji przez QA. Zakres i wyniki dopiero po wykonaniu.
2. **P1 — trwałe migracje w domyślnym CI:** modelowy pakiet jest odebrany;
   PCB także ma odebrany trwały pakiet: 64 payloady / 241963 bajty,
   native 5840, niezależnie 96 odczytów / 144 rekordy bez błędów i 9 odmów. Wspólny
   runner i dwa domyślne CTest pozostają tylko staged. Brak zewnętrznych modeli/oracle, izolowany
   project-build temp, zachowanie dowodów błędu.
3. **P1 — kolejne piony:** niezastosowany wrapper/target libvgcode z własnym
   rzeczywistym testem architektury; później DSForGcode/cache, profile,
   liczbowe SVG z restartem/slicingiem i pozostałe formaty/operacje UVtools.

Preview 3D pokazuje model, statystyki mogą korzystać z edytowanych masek,
a pełne aktualne piksele ogląda się w Sliced. Nie odebrano rekonstrukcji
mesha 3D z PNG ani automatycznej naprawy offscreen/DPI. Ostatni znak <1%
legendy ma dowód wizualny, nie ścisły OCR. Źródła i wyniki są szczegółowo
opisane w [raporcie preview](M2_SLA_PREVIEW_OWNER_2026-10-16.md).

[Zachowana chronologia wcześniejszego frontu](M2_CURRENT_FRONT_HISTORY_2026-10-16.md)
zawiera dawne stany pośrednie. Nie są one bieżącymi blokadami lub nowymi PASS.

Historyczne sekcje poniżej opisują własne źródła i wyniki. Nie zostały
przemianowane na dowody bieżącego binarium; oryginalne RED i błędy QA zachowano.

## Historyczny buildc8139d79 —2026-10-16, runtime pending

All-target74365 exit0; app
`c8139d79c2a42e6e63a339ff7783ae9c5bf5b492f4326be9989416ea472ac5d5`,
Plater `22b7044b…` zawiera parse-once DTO/guard przed destructive reset;
MainFramec928 i test148b bez zmian. Profiles wykonuje pełny strict GUI:
odmowa FFF zachowująca baseline, geometry-only import oraz prawidłowy SLA
reload6masks. **Brak jeszcze runtime PASS/final seal c813.** Native12/1140/4
poniżej należy do3dee-era/test148b i nie jest nowym uruchomieniem c813 GUI.

## Poprzedni3deeb64f —2026-10-16, rzeczywisty legacy RED

**Owner nadal WIP, bez final seal.** Zamrożona app
`3deeb64f7214c388ab5c6933d5a77e39681c4e93dab7a7d9ad61330da229397a`,
Plater013a/MainFramec928; all-target58869 exit0, test rebuild27865 exit0.
Historyczny305c poniżej pozostaje osobnym odebranym archiwum.

Cztery rzeczywiste retained-SLA przypadki SL1/CXDLPv4 × podpory/pad off/on
PASS: **161334 asercje /4 uruchomienia**. Oryginalny UVtools oraz poprawiony
niezależny checker: **12 plików /1140 rekordów /4 projekty PASS**. Początkowy
błąd importu QA zachowano osobno; nie był wadą eksportu. Dowody:
`build/fff-owner-removal-2026-10-15/retained-sla-runtime-v2/`.

**Rzeczywisty produkcyjny legacy Open RED:** po zamknięciu prawidłowego
natywnego chooser istniejąca edytowana scena2×1×0.3 została zastąpiona
modelem3×5×7 i tytułem `legacy-fff-authored`, bez odmowy FFF. Proces zakończył
normalnie0, INET0 i tożsamości zgodne. Wcześniejszy Import Cancel zachował
baseline. To dowód zmiany sceny, nie samo oczekiwanie na selector. Root
archiwizuje ten actualRED i wdraża rebased parse-once DTO/guard przed reset;
nowy build/runtime GREEN jeszcze nieodebrane. [Review](../build/sla-session-owner-2026-10-15/legacy-transaction/REBASE_AND_GATE_REVIEW.md).

Prywatne initial nonmax WindowMetrics1280 umożliwia natywny Slice/Sliced/
Invert/save6PNG; to setup QA, nie naprawa auto-restore/offscreen/DPI. Dawny
blank/nonSHOWING1×1 występował także na305c w nowym QA/env, więc przyczyna
nie jest ustaloną regresją ownera. Tempdiagnostyka wx/GTK usunięta z produkcji,
oryginalne patch/app/log zachowane; cachepatch nadal niezastosowany.

## Checkpoint 305c4b04 — scoped_pass, pełny goal OPEN

App `305c4b04…`, testy `cf4bff9e…`: all-target build exit0; **376990/55
PASS**, CTest **10/10, 76.79s**, FFF7 i ELF TBB PASS. Siedem niezależnych
weryfikatorów: **301 odczytów plików / 759 rekordów / 153 odczyty projektów
PASS**, w tym merged PCB **48/48/16**. Czysty cancel RED **5/18347** oraz
wcześniejszy błąd reportera Catch zachowane osobno.

Rzeczywisty PM7 GUI generate/save/restart/export dla dwóch scalonych,
odwróconych kół: oba procesy PASS za pierwszym podejściem, dwa zwykłe
wyjścia0, INET0. Oryginalny UVtools/PW0: **2 pliki / 2 rekordy**, dokładne
maski/geometria/13 pól/raw rows,14 pośrednich poziomów Gray4 i literalne
**331s**. To nie test GUI anulowania ani GUI wszystkich4 ZIP. QA celowo
dopasował/przesunął okno1280×800; nie jest to dowód automatycznej naprawy
geometrii offscreen/DPI. [Raport GUI](M2_PCB_PM7_GUI_ACCEPTANCE_2026-10-15.md).

Seal: `build/pcb-merged-cancel-2026-10-15/final-acceptance.json`.
31 pinów:29 kopii SHA źródeł/binarnych, model/notatki wyłącznie hash-only;
GUI QA zarchiwizowane osobno,44 artefakty SHA sprawdzone, identity-before=after.
Jedyna produkcyjna zmiana względem9e to forwarding callbacku PCB; bez zmiany
pikseli/schema/API. Historyczny odbiór pędzla9e pozostaje odrębnym dowodem,
nie nowym probe na305c. Kolejny source epoch może już różnić się od seal.

| Pion / architektura | Stan | Następna bramka |
|---|---|---|
| PCB / merged + cancel | DONE sprawdzony zakres | Backend callback/cancel oraz PM7 GUI/native odebrane; pełne PCB/Gerber/Excellon, hardware i GUI latency nadal OPEN. |
| Live FFF owner / sesja SLA | IN PROGRESS — tagged GUI PASS, missingtech/final gates OPEN | App3cc0 strictCancel/FFFrefusal exactbaseline, geometry-onlyImport, validSLA6PNGrestore PASS. Coldwizard/savedrestart PASS w swoim zakresie. Untagged config nadal wymaga rzeczywistego RED/guard; finalCTest/audyty/seal pending. [Review](../build/sla-session-owner-2026-10-15/OWNER_RUNTIME_PENDING_REVIEW.md). |
| Wydajność podpór / geometria | Odebrany wcześniejszy zakres; dalsze P0 OPEN | Bez nowego benchmarku305c. Plan półwyspów/testy krawędzi staged-only; brak gwarantowanego whole speedup. |
| Presety / probe dostępności GUI | QA korekta selektora, brak dowodu błędu produktu | Dokładne drzewo AT-SPI udostępnia3 wybrane nazwy w DESCRIPTION paneli obok etykiet ról. Wcześniejszy lookup combo.value był błędem QA; pierwotne całe probe FAILED zachowane. Ścisły ponowny probe korzysta z natywnych DESCRIPTION + zapisanej konfiguracji. |
| Startup / pozycja okna | P1 OPEN UX | Rzeczywisty screenshot pokazuje częściowo offscreen odtworzone okno. Ręczne dopasowanie QA nie naprawia produktu; wymagany guard geometrii względem dostępnego ekranu i testy DPI/restore. |
| Integracja / release / pełny goal | Scoped checkpoint zamknięty, goal OPEN | Wszystkie formaty/parametry/profile/UI/UVtools, dalsze FFF, pakowanie offline/platformy/DPI/hardware wymagają dalszych kart. |

Poniżej niezmienna historia9e i wcześniejszych source epochs.

## Checkpoint 9e015e04 — 2026-10-15, scoped_pass

- Build/core **358643/53 PASS**, CTest **10/10 (76.74s)**; sześć niezależnych
  odczytów UVtools **253/711/137 PASS**, w tym PCB48/96/16 +8 v1 odmów.
- Pędzel source LUT/hint/preview/commit/range/history/restart/readback odebrany
  dla18/2. Oryginalny cały binary probe pozostaje FAILED przez błędne oczekiwanie
  suffixu QA; jego ukończona faza + poprawiony restart2 i oryginalny readback
  mają osobny jawny odbiór. Nie przepisano starego raportu ani31 tożsamości.
- GUI:4 rzeczywiste pliki/12 rekordów +2 referencje/6 rekordów PASS;
  6 zwykłych wyjść0, INET0. QA ręcznie ustawił okno1280×800 — nie automatyczne
  offscreen recovery ani pełne DPI/platformy.
- Archiwum29 źródeł/binarnych +4 osobne poprawione GUI QA i98 artefaktów SHA;
  app9e015e04…, test9b3cc7a0…. Model/notatki użytkownika hash-only.
  Seal: `build/brush-pcb-integration-2026-10-15/final-acceptance.json`.
- FFF7/ELF TBB PASS. Clipping pozostaje odebranym wycinkiem fazy; brak nowego
  benchmarku9e i gwarantowanego przyspieszenia całego slicingu.
- **OPEN:** dalsze PCB/cancel/Gerber/Excellon, pełne formaty/parametry/profile/
  operacje UVtools, FFF Print owner, pakowanie offline, platformy/DPI/sprzęt.
  Staged compiler RED i test geometrii półwyspów nie zostały zastosowane.
  [Bieżący kanban](KANBAN_WDROZENIA_2026-10-11.md). Poniżej historia eaba6a6d.

## Checkpoint eaba6a6d — 2026-10-15, scoped_pass

- All-target build exit0, focused323431/34, CTest10/10 (76.19s);
  oryginalny UVtools205 plików/615 rekordów/121 odczytów projektów PASS.
- Odebrano zakres niezmiennej palety PNG/projekt8, ciągłości kontraktu przez
  guarded Apply oraz ograniczonej CPU równoległości clippingu. Full formaty,
  parametry i główny goal pozostają otwarte.
- Ramka27.1822s/live export2.8970s/frozen3.0357s, clip5.3261s;452 dokładne
  podpory/213 RLE/42 globalne pola zgodne. Poprawa fazy clippingu jest spójna;
  brak gwarantowanego przyspieszenia całego slicingu. Grain1 odrzucony.
- **P0 IN PROGRESS: informacja o efektywnej palecie pędzla** — implementacja
  w integracji; rzeczywisty GUI RED i compiler API RED exit2 zachowane, bez GREEN;
  brak127→119/0; wymagane zgodne źródło, podgląd, stroke/UndoRedo/restart/readback.
- **P0 IN PROGRESS: paleta źródłowa PCB** — projekcja przed PNG w integracji,
  GREEN nieodebrane; runtime RED292/26788 asercji/2 przypadki;
  UVtools48 plików/96 rekordów/16 projektów:224 oczekiwane błędy palety/pikseli/
  schema/kontraktu. Osiem v1 odmów chronionych PASS; poprawka/GREEN wymagane.
- Nawigacja do błędnego pola ma historyczny GUI PASS4ab4df73 z tym samym kodem,
 3 przypadki/normalne wyjścia/INET0; nie jest to pełny ponowny GUI eaba6a6d.
- Archiwum: `build/model-mask-contract-2026-10-15/final-acceptance.json`,
 22 tożsamości i24 kopie źródeł/binarnych; model i notatki użytkownika hash-only.
- [Aktualny kanban](KANBAN_WDROZENIA_2026-10-11.md),
  [pomiary](M2_SUPPORT_CLIP_PARALLELISM_2026-10-15.md).
  Live FFF Print owner: compiler RED patch READY, niezastosowany/niewykonany.

Poniższe wpisy są historią własnych checkpointów, nie aktualnym odbiorem źródeł.

## Odebrany checkpoint 2f398a92 — 2026-10-15

- Wszystkie targety zbudowane, CTest10/10 (75.92s), focused190583/19.
- Palety początkowych modeli6ID: UVtools60plików/180warstw/24projekty PASS;
  binaryv1 kalibracje12/36/8 PASS. Brak deklaracji pełnych parametrów/hardware.
- Edycja wiersza i osobny rzeczywisty restart:2UVJ/28rekordów, dwa exit0/INET0.
- Ramka użytkownika27.131s/export2.863s;452dokładne podpory/213RLE/42pola zgodne.
  Grain1 nie wykazał przyspieszenia całego procesu, przywrócono8.
- Nowy etap: rzeczywisty RED dowolnych szarości/projektu8, naprawa ujawniania
  błędnych pól po rzeczywistym GUI RED, kontrolowany opt-in eksperyment8/16.
- Kanban: [bieżąca tablica](KANBAN_WDROZENIA_2026-10-11.md). Źródła kolejnego
  etapu mogą już różnić się od zamrożonego checkpointu; pełny goal otwarty.
  Poniższe wpisy są historią i nie zastępują aktualnych dowodów.

## Bieżąca praca po checkpoint ae3eab0f — 2026-10-15

- Binaryv1 kalibracje: core i niezależny UVtools PASS12plików/36rekordów/
  8projektów; próg255 i historyczna precyzja maski bez nadawania ownership.
- Wydajność: opt-in pomiar wskazał clipping6.087s (99.18% wyrównywania).
  Grain1 zachował dokładnie452podpory i213RLE, lecz pojedynczy pomiar całego
  slicingu nie wykazał poprawy. Eksperyment nadal w ocenie.
- Dostępny selektor i modal edycji wiersza skompilowane; rzeczywisty odbiór
  GUI/restart/readback trwa. Błędy parsowania zachowują modal i szkic.
- **Nowe P0 z audytu:** paleta zwykłych modeli legacy nie korzysta jeszcze
  z niezależnej zdolności maski użytej przez generator kalibracji. Rzeczywisty
  RED ModelSlice przygotowany jako patch; brak deklaracji naprawy.
- [Bieżący raport](M2_LEGACY_PALETTE_ACCESSIBLE_ROWS_AND_SUPPORT_PROFILE_2026-10-15.md)
  i [kanban](KANBAN_WDROZENIA_2026-10-11.md) rozróżniają wyniki od bramek otwartych.
  Nowa integracja nie ma jeszcze końcowego CTest; poniżej checkpoint historyczny.

## Bieżąca integracja — 2026-10-15, checkpoint ae3eab0f

- Odebrane:7obiektów emitera FFF poza buildem SLA; czas Anycubic ZIP z natywnych
  parametrów; izolacja ELF statycznego TBB po odtworzonym SIGSEGV finalizacji;
  stan Needs normalization w tabeli. [Raport](M2_FFF_TIME_AND_RUNTIME_ACCEPTANCE_2026-10-15.md).
- CTest10/10,78.19s; focused21601/37; niezależne73pliki/219rekordów/65projektów.
  GUI: pędzel/range/historia/zapis, główny Sliced, tabela i PM7/restart PASS.
 6procesów/audyt INET0,4zwykłe zamknięcia exit0. Ramka27.865s/export3.200s;
 213RLE i wszystkie sprawdzane parametry zgodne, bez deklaracji speedup.
- Komponent pakowania obsługuje jawne biblioteki dlopen i SONAME:14testów PASS.
  To jeszcze nie rzeczywisty pakiet ani kompletna relokacja/notices/installer.
- Bieżące zadania: pomiar faz dopasowania podpór; RED palet kalibracji binaryv1;
  dostępny selektor i edytor wiersza. Pełne formaty/parametry/profile/PCB/suite
  kalibracji/operacje, pozostałe FFF, DPI/platformy i sprzęt nadal otwarte.
- [Kanban](KANBAN_WDROZENIA_2026-10-11.md) jest bieżącą tablicą. Checkpoint przypięty
  w `build/fff-dependency-cut-2026-10-15/final-acceptance.json`; dalsze źródła
  w trakcie pracy mogą już różnić się od odebranego binarium. Starsze wpisy
  poniżej opisują historyczne wyniki i otwarte wówczas bramki.

## Aktualizacja V22/V10/V11 — 2026-10-15, rzeczywiste wzorce ExposureFinder

- Natywne built-in patterns: holes/bars/bullseye/triangles/fences, AA i klasyczne
  czcionki. Produkcyjny create_exposure_finder_calibration łączy je ze schedule,
  layoutem i owned SLAPrint. Bez siatki i zewnętrznego procesu UVtools/.NET.
- OpenCV core/imgproc jest lokalną zależnością C++; klasyczne dane fontów i ich
  metryki są własnymi plikami projektu. Naprawiono niezgodności OpenCV5 fontów,
  krawędzi prostokątów, double TextScale i cienkich kresek subpixel.
- Oryginalne GetLayers:60konfiguracji/120płaszczyzn dokładnie zgodnych, także
  italic, ułamkowe wartości, nierówny pitch i UTF-8. Core2249asercji/15przypadków
  PASS, z walidacją pustych wejść, draft isolation, cancel i progress0–100.
- UVtools/Pillow/JSON16plików UVJ/CXDLPv4,16projektów,196rekordów PASS po restore;
  maski/LCD/mm/Z/13parametrów i surowa fizyczna union volume CXDLPv4 zgodne.
  Syntetyczny LCD i identyfikatorCL-60 nie stanowią konfiguracji sprzętowej.
- Aktualny GUI/restart2procesy/network0 +independent5UVJ/10rekordów PASS.
  To regresja istniejącej sesji, nie nowy formularz generatora.
- App/testbuild0, CTest10/10,74,77s; frozen identities i protected SHA PASS.
  Ramka użytkownika: slicing28,248s/export2,898s;213RLE i odczyt UVtools zgodne
  z baseline poza UTCminute. Brak deklaracji nowego speedup z jednego pomiaru.
- Komponent pakowania:164kopie bibliotek,163rozwiązania wewnątrz projektu plus
  zgodny interpreter OS. Naprawione granice ścieżek,4testy Python PASS. Pełny
  installer/launcher/resources/notices/CPU/ABI/relokacja nadal otwarte.
- Następne: pełny typed recipe pattern/layout i formularz/controller w Sliced;
  osobno PatternModel/glue, interpretacja/profile apply oraz reszta kalibracji.
  Pełny goal, formaty, profile, PCB i dalsze performance pozostają otwarte.
  [Raport i dowody](M2_EXPOSURE_FINDER_PATTERNS_2026-10-15.md).

## Aktualizacja V22 — 2026-10-15, schodki/erozja/fazowanie ExposureFinder

- Natywny renderer dwóch płaszczyzn wzorca: malejące schodki według tested height,
  bottom erosion i chamfer, przed napisami/brightness. Własne immutable wejścia,
  walidacja wysokości i anulowanie przez rzeczywisty kompozytor LCD.
- Runtime RED8/188 w5przypadkach → GREEN725asercji/11przypadków z wcześniejszym
  kompozytorem. Naprawiono brak walidacji oraz przesunięcie schodka o piksel:
  dzielenie/ceil odbywa się na całkowitych mikrometrach harmonogramu.
- Niezależny Emgu/OpenCV144/144obrazów erozji PASS, także oś1px i duże kernele.
  QA jest offline i poza produkcyjnym buildem; bez nowej zależności aplikacji.
- App/testbuild0, GUI/restart2procesy/network0, UVtools5UVJ/10rekordów PASS
  dla istniejącej sesji. CTest10/10,71,56s; frozen identities/protected SHA PASS.
- Pełne kontury/napisy, AA nowo rysowanych schodków, recipe/layout oraz generator
  i formularz nadal otwarte. To nie odbiór kompletnego ExposureFinder ani sprzętu.
  [Raport](M2_EXPOSURE_FINDER_CELL_2026-10-15.md).

## Aktualizacja V22/V11 — 2026-10-15, layout LCD i kompozycja ExposureFinder

- Odebrany backend: kompletne rozmieszczenie footprintów w fizycznej geometrii LCD,
  orientacja/centrowanie/odbicia, saturating AA brightness, walidacja i atomic result.
  Wymaga prawdziwego renderera komórki; pełne wzorce i GUI generatora są otwarte.
- PNG row cancellation w bieżącym encoderze projekcji; sprzątanie wyjątków po
  libpng longjmp i zwalnianie kolejnych własnych kopii PNG w from_rasters.
- RED2/3 → GREEN545asercji/6przypadków. Niezależne Pillow128PNG/16wariantów;
  wszystkie4kanałowe tryby zgodne bajtowo z zamrożonym baseline encodera.
- GUI/restart2procesy/network0, UVtools5UVJ/10rekordów PASS. Są to testy
  dotychczasowej sesji, nie eksportu pełnych wzorców nowego kompozytora.
- App/test build0; CTest10/10,71,79s. Bieżąca ramka: slicing29,384s,
  liveCXDLPv4export2,828s;213RLE/metadane identyczne pozaUTCminute.
  OriginalUVtools213rekordów/all exposed fields zgodne z odebranym baseline.
  Model tylko odczytany z Downloads, nie skopiowany; protectedSHA PASS.
- Pełny goal oraz dalsze performance/UI/formaty pozostają otwarte.
  [Raport](M2_EXPOSURE_FINDER_RASTER_2026-10-15.md).

## Aktualizacja V22/V16 — 2026-10-15, trwały harmonogram ExposureFinder

- ExposureFinder descriptor v2 zapisuje wszystkie14pól opcjonalnego schedule.
  Semantykę sprawdza wspólny planner, bez rekurencji walidacji. Clone obejmuje
  również listę brightness; alias szkicu nie zmienia SLAPrint/ExportSnapshot.
  Disabled wartości są zachowane, a legacy v1 nie dostaje domyślnego harmonogramu.
- Sliced recipe details pokazuje wszystkie opcje/stany/jednostki czytelnymi liczbami.
  To podgląd read-only, nie nowe tworzenie/edycja pełnego generatora.
- Runtime schema RED→**920assertions/21cases PASS**, w tym38nowych hostile mutacji,
  wszystkie5base×3exclusion roundtrip, raw custom precision, inactive fields,
  legacy absent, target preservation i restore bez regeneracji masek.
- GUI Auto/Manual/legacy +clean restart3workflow/6process/network0 PASS.
  Independent UVtools/Pillow/JSON13uniqueUVJ/26records PASS (2baseline+11nowych).
  Release build0, CTest **10/10,71,55s** i protected SHA PASS. Model bez kopii.
- Synthetic fixture nie jest generatorem ExposureFinder. Następne: pełne wzorce/
  layout, owned raster generation, GUI/table editing, interpretation/profile apply.
  Pełny goal/formaty/profile/pozostałe UVtools/PCB/performance/release nadal otwarte.
- [Raport i zależności](M2_EXPOSURE_FINDER_SCHEDULE_RECIPE_2026-10-15.md).

## Aktualizacja V22 — 2026-10-15, fizyczny schedule ExposureFinder

- Dodano native plan Z/physical groups/parts/exposure, bottom/base i brightness
  exclusions, ordered base selectors/Custom, same-Z lift/wait overrides.
  Sparse events, input snapshots, resource bounds i cancel/FP restore. Mask
  brightness nie zmienia czasu cure. Wysokość testu odróżniona od globalnego Z.
- Runtime RED→**687assertions/17cases PASS**. Oryginalny UVtools Execute rzeczywiście
  wygenerował46konfiguracji/539rekordów; sprawdzono Z/czasy/counts/lift/wait oraz
  wszystkie nonzero gray values masek. Time-grid differential fix utrwalony.
  Jedna jawna poprawka reference: CustomBase działa także dla jednego testu
  (native3,25s vs reference2s). Verifier wymaga dokładnie opisanej różnicy.
- Release app/test/oraclebuild0; CTest **10/10,71,56s**. Regresja recipe GUI
  2process/network0 i independent5UVJ/10records PASS (2baseline+3noweGUI).
  Chronione SHA zgodne, model bez kopii. Dowody i source/binary identities zapisane.
- **Kernel nie jest jeszcze pełnym generatorem.** Następne: schedule recipe
  persistence, pełne geometry/layout, owned raster generation, GUI/table editing,
  profile apply i dalsze native exports. Pełny goal/pozostałe wymagania bez zmian.
- [Raport i zależności](M2_EXPOSURE_FINDER_SCHEDULE_2026-10-15.md).

## Aktualizacja V22/V16 — 2026-10-15, trwały przepis ExposureFinder

- CalibrationRecipe v3 zawiera typed ExposureFinder descriptor v1: parametry,
  origin Generated/Manual i pełną tabelę. Generated musi odpowiadać generatorowi;
  Manual zachowuje kolejność/duplikaty/invalid draft rows, z co najmniej1valid.
  Parser odrzuca unknown/missing/type/range/downgrade/stale metadata.
- SLAPrint deep-clone chroni również nested mutable aliases i ExportSnapshot.
  Restore zachowuje maski/parametry bez regeneracji. Basic generator jawnie
  odmawia nowego rodzaju. W Sliced działa read-only review recipe (do100rows).
- Runtime schema RED → **1095assertions/16cases PASS**, w tym40hostile mutations,
  target preservation, legacy recipe/taper i ownership. GUI Auto/Manual/restart
  oraz stary generator3workflow/6process/network0 PASS. OriginalUVtools/Pillow/
  JSON10uniqueFiles/56records PASS. Release build0; CTest **10/10,71,46s**.
  Chronione SHA zgodne; model bez kopii ani modyfikacji.
- **Fixture jest syntetyczny, to nie pełny generator ExposureFinder.** Następne:
  physical schedule, pełne wzorce/layout, tworzenie/edycja tabeli w GUI,
  interpretation/profile apply i dalsze preserving exporters. Pełny goal aktywny.
- [Raport i zależności](M2_EXPOSURE_FINDER_RECIPE_2026-10-15.md).

## Aktualizacja V22 — 2026-10-15, rdzeń tabel ExposureFinder

- Dodano natywne API ExposureFinderTable: linear/multiplier z czynnikiem height,
  wiele wysokości, base on/off,0–255tests, decimal ties-even/height-away rounding,
  test dedup i manual sanitize/sort/validity. Input snapshot, bounded100000rows,
  cancel i pełne przywracanie FP. Bez runtime UVtools i zmian profili.
- TDD RED2/2 → final **387assertions/8cases PASS**. Niezależne wykonanie metod
  oryginalnego UVtools: **297cases/2957rows exact PASS**. Differential RED wykrył
  powtarzane base rows przy kroku0,0004mm; zachowanie poprawione i test utrwalony.
  Pierwszy błąd reflection Decimal/Byte dotyczył wyłącznie narzędzia QA.
- Release app/testbuild0; CTest **10/10,70,23s**. Regresja starego generatora
  Sliced/save/restart2process/network0 i independent2files/40records PASS.
  Chronione SHA bez zmian, model bez kopii. Dowody/tożsamości zapisane.
- **To jeszcze nie generator ExposureFinder w GUI.** Następne: owned recipe,
  harmonogram bottom/base/features/brightness, pełne wzorce/layout, GUI,
  interpretacja i profile apply. Cały goal/pozostałe wymagania pozostają otwarte.
- [Raport i zależności](M2_EXPOSURE_FINDER_TABLE_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, CXDLPv4 i kilka ekspozycji na jednym Z

- Jawne sekwencje RepeatedExposureHeights można eksportować do CXDLPv4 obok UVJ.
  Każdy rekord zachowuje maskę/Z/parametry; union liczy objętość raz na fizyczną
  warstwę. Projection guard chroni grupy i grid0,001mm; zwykły model count path
  zachowany. Pozostałe formaty wymagają osobnego wdrożenia tej polityki.
- Runtime RED odmowy → core **2366assertions/14cases PASS**; 16wariantów
  orientation/direct-restart/bottom/producer, batch-boundary union, cancel
  po staging, target preservation/cleanup/retry i precision refusal.
- GUI z ustawień → Sliced, wybór CXDLPv4/default suffix, edycja/save/reload/
  Recent/restart/close PASS. Zwykły model brush/range/Apply/3MF/restart/error/retry
  PASS. 2workflow/4procesy/network0; originalUVtools21files/86records PASS.
  Release build0, CTest **10/10,70,27s**, chronione SHA zgodne.
- Pełny ExposureFinder, pozostałe native writers/repeated-Z, profile/settings,
  dalsze UVtools/PCB/kalibracje, wydajność i niezależny release nadal otwarte.
  Brak nowego pomiaru przyspieszenia i odbioru firmware.
- [Raport](M2_CXDLPV4_EXPOSURE_SEQUENCES_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, Recent sceny 3D i przerwany gest pędzla

- Naprawiono modalną obsługę Recent projects: odroczenie z kopią ścieżki,
  kontrola indeksu, usuwanie brakującego wpisu po nazwie. Runtime RED dostępności
  GTK → GREEN Cancel/Discard; starsze maski i historia zachowane po zmianie sceny.
- Nowy rzeczywisty held-mouse test switch/replace/close potwierdza anulowanie
  niedokończonego gestu, zwolnienie capture i zachowanie zatwierdzonych pikseli.
  Brak odtworzonego defektu pędzla na tym GTK; bez zmian mechaniki produkcyjnej.
- 5 workflow / 8 audytowanych procesów GUI / network0; 9 raportów, niezależny
  UVtools (10 plików / 88 rekordów), dokładne PNG i chronione SHA PASS.
  Release build0; CTest **10/10, 69,98 s**.
- M5/V18 i pełny goal nadal IN PROGRESS: pozostała macierz scene IO/settings,
  failure/cancel/busy/late events, duże maski/OS/DPI; formaty, profile,
  UVtools/PCB/kalibracje, performance, FFF cleanup i dystrybucja pozostają otwarte.
- [Raport](M2_STROKE_AND_SCENE_RECENT_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, jeden host pierwszego i kolejnego Edit layers

- Pierwsze sidebar Edit layers również otwiera stały Sliced. Back to 3D zachowuje
  stan i historię; bieżący wynik wraca do tego samego panelu. Inny dokument nadal
  wymaga No/Yes. Powrót z Printer Settings ujawnia Plater/Sliced także dla
  już istniejącej sesji. Runtime RED modal i ukryty powrót → GREEN.
- Usunięto klasę modalnego hosta, oba show_layer_editor, dialog_host flags,
  tytuł/Close hosta i m_source_print. Panel pobiera kontekst wyłącznie z własnej
  sesji; Apply używa chronionego kontrolera. Dialogi parametrów/narzędzi pozostają.
- Final real stroke/range/Undo/Redo/Apply, parametry7.12, Back/reopen/settings,
  3MF/restart, CXDLPv4/error/retry i SL1 PASS. Regresje workspace/history/archive/
  generator i niezależne maski/parametry PASS. 6 workflow / 9 audytowanych
  procesów, 0 prób sieciowych; build0, CTest **10/10, 69,96 s**. 12 raportów i
  tożsamości w final-acceptance.json. Chronione SHA bez zmian; model bez kopii.
- **Następne P0:** pełne New/Recent/Open sceny 3D i układy ustawień, wiele
  dokumentów, close/save/cancel/busy/late events/failure, aktywny stroke/capture.
  M5/V18 nadal IN PROGRESS. Wymagania wszystkich formatów/drukarek, profili,
  UVtools/PCB/kalibracji, performance, FFF cleanup i offline release bez zmian.
- [Raport](M2_PRIMARY_SLICED_EDITOR_2026-10-15.md); manual i kanban zaktualizowane.

## Aktualizacja P0 — 2026-10-15, menu i historia dokumentów warstwowych

- Window/Open layer archive otwiera stały Sliced, również z ustawień drukarki.
  File/Recent layer documents ma osobną listę do 9 pozycji, dodawaną po udanym
  otwarciu lub publikacji projektu. AppConfig/C-style zachowuje Unicode/spacje/&;
  historia 3D pozostaje osobna. Brakujący wpis jest usuwany po komunikacie,
  a otwarty dokument i jego dane pozostają zachowane.
- Failed import po Yes nie porzuca wcześniejszych pending masks/history/baseline
  i nie dodaje wadliwego pliku do Recent. New Project zmienia scenę 3D, zachowując
  niezależny dokument Sliced. Historia odtwarza projekt po czystym restarcie.
- Runtime RED: menu otwierało modal; regresja odczytu CRC error po konsolidacji
  miała szczegół w HTML ogólnego error. Proste komunikaty missing/import są
  czytelne i dostępne przez accessibility. Wcześniejszy błąd wrapper source path
  (przed startem aplikacji) zapisany osobno, nie zaliczany jako RED produktu.
- Final SL1/CXDLPv4/PNG/generated, history/New/restart/failed import i regresje
  archive/model/modal/generator PASS. 9 workflow / 14 audytowanych procesów,
  0 prób sieciowych; independent FullOpen/piksele/parametry i binarny archive
  match PASS. Build0, CTest **10/10, 70,00 s**. 17 raportów/tożsamości i chronione
  SHA zgodne; model użytkownika bez kopii ani modyfikacji.
- **Następne P0:** Edit layers bez istniejącego panelu, pełna macierz 3D
  New/Recent/Open i układów ustawień, wiele dokumentów, close/save/cancel/busy/
  late events/failure/restart. M5/V18 nadal IN PROGRESS. Wszystkie wymagania
  formatów/wersji, profili, UVtools/PCB/calibracji, performance, FFF cleanup
  i niezależnej dystrybucji pozostają aktywne.
- [Raport](M2_LAYER_DOCUMENT_HISTORY_2026-10-15.md); kanban i manual zaktualizowane.

## Aktualizacja P0 — 2026-10-15, główne polecenia dokumentu Sliced

- W Sliced dla archiwum/obrazu/generatora główne Save/Ctrl+S zapisuje własny
  `.sla-project`, Save As ustawia nową ścieżkę, a Open Project otwiera picker
  warstwowy. Główny Export korzysta z edytora również dla wyniku modelu i starszej
  zachowanej sesji. Wewnętrzny zapis sceny 3D przy close pozostaje osobny.
- Przycisk i menu mają wspólny helper Save oraz callback Export. Ścieżka i
  baseline zmieniają się po publish; Cancel/failure zachowują dane. Save As
  nie nadpisuje starego projektu przy następnym Ctrl+S.
- Edit layers przy obcym/starszym panelu proponuje bieżący wynik modelu.
  No zachowuje poprzednie edycje i widok; Yes publikuje udany capture.
- Runtime RED: Save As wyświetlał pytanie o pustą scenę 3D (zachowany tree).
  Final SL1/CXDLPv4/PNG/generated, rzeczywisty Ctrl+S, znana ścieżka, Save As
  Cancel, failed publish/staging cleanup/retry, main Open/Export oraz niezależne
  piksele/parametry PASS. Regresje foreign/model/modal/generator PASS.
  7 workflow / 9 audytowanych procesów, 0 prób sieciowych; build0,
  CTest **10/10, 70,17 s**. 14 raportów/tożsamości w final-acceptance.json;
  SHA oryginalnego modelu i notatek bez zmian, bez kopiowania siatki użytkownika.
- **Następne P0:** pełna konsolidacja otwierania i edycji (modalne ścieżki,
  Recent/New/inne zakładki, wiele dokumentów), close/save/cancel/busy/late events/
  failure/restart. M5/V18 nadal IN PROGRESS. Dalej pełna macierz formatów i wersji,
  UVtools/PCB/kalibracje/profiles/performance/FFF cleanup/offline release.
- [Raport](M2_SLICED_COMMAND_ROUTING_2026-10-15.md); manual i kanban zaktualizowane.

## Aktualizacja P0 — 2026-10-15, istniejące generatory otwierają stały Sliced

- Generate PCB layers i Generate calibration layers publikują własną sesję
  w Sliced. Tymczasowy job jest zwalniany; maski, konfiguracja, Z i przepis
  pozostają w edytorze. Brak Apply do modelu. Nowy wynik jest chroniony przed
  porzuceniem także przed pierwszą edycją i ma komunikat o braku zapisu/eksportu.
- Regenerate No zachowuje wcześniejsze edycje/historię oraz główną zakładkę;
  Yes publikuje nowy wynik po capture. Sukces ujawnia Plater/Sliced również
  przy uruchomieniu generatora z Printer Settings.
- Runtime GUI RED: modalny edytor zamiast Sliced; następnie istniejący, ale
  ukryty wynik z zakładki ustawień (potwierdzone accessibility tree). Oba GREEN.
- Final 8 workflow / 14 audytowanych procesów, 0 prób sieciowych. Kalibracja
  normal/settings, lift 11+9, PCB landscape/portrait, projekty/restart oraz
  regresje model/modal/plik i niezależny UVtools/piksele/parametry PASS.
  Build0, CTest **10/10, 70,00 s**. 16 raportów i tożsamości w final-acceptance.json.
  SHA oryginalnej ramki i notatek bez zmian; bez kopiowania modelu użytkownika.
- **Następne P0:** główne Save/Open/Export aktywnego dokumentu oraz Edit layers,
  gdy Sliced zawiera plikowy lub starszy wynik; wymagany runtime RED i odbiór
  save/cancel/error/restart. Dalej pełna macierz busy/late events/capture cancel,
  duże maski/profiles/formaty/OS/DPI. M5/V18 nadal IN PROGRESS.
- Nie zamknięto pełnego UVtools/PCB/calibration suite, wszystkich drukarek,
  CRUD profili, wydajności, usunięcia pozostałości FFF ani dystrybucji offline.
  [Raport](M2_GENERATED_SLICED_WORKSPACE_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, pliki jako samodzielne dokumenty Sliced

- **Open layer file** w stałym Sliced otwiera archiwum, obraz lub `.sla-project`
  bez modelu. Wspólny loader obsługuje panel i dotychczasowy dialog. Własne
  ustawienia projektu generatora pozostają niezależne od aktywnej drukarki.
- Samodzielny dokument ma własny Save layer project, eksport i historię;
  nie udaje starszego wyniku modelu i nie ma Apply do sceny. Zastąpienie
  następuje po udanym wczytaniu. Cancel i No zachowują wcześniejszy dokument.
  Zamknięcie chroni go osobno po decyzjach dotyczących sceny/presetów.
- Runtime RED brak Open layer file → GREEN dla SL1/CXDLPv4/PNG/calibration-project.
  Pośrednie timeouty fixture menu/readiness/standardowych pytań zapisano osobno;
  nie są zaliczane jako błędy produktu. Pełny build0 i CTest **10/10, 70,08 s**.
- Final GUI 7 workflow / 9 audytowanych procesów, 0 prób sieciowych. Niezależne
  piksele, geometria, Z, parametry i materiał oraz regresje model/modal/generator
  PASS. 14 raportów i tożsamości w final-acceptance.json; chronione SHA bez zmian.
- **Następne P0:** nowo generowane PCB/kalibracje → Sliced; pełna macierz import
  failure/capture cancel/late events/disk/zmiany profili, także PCB repeated-Z.
  **P1:** główne Save/Open aktywnego dokumentu, ograniczenie pytań o pustą scenę
  i presety, dwa uchwyty/zoom/planes/duże maski/OS/DPI. M5/V18 nadal IN PROGRESS.
- Pełna macierz formatów i drukarek, UVtools, PCB/kalibracje, CRUD profili,
  wydajność, FFF cleanup i niezależna dystrybucja pozostają wymagane.
  [Raport](M2_SLICED_FILE_WORKSPACE_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, stały widok Sliced dla modelu

- **Odebrany zakres:** rzeczywiste maski w trzecim widoku obok 3D/Preview,
  zachowane edycje/narzędzie/zakres/Undo, brak ponownego slicingu przy zmianie
  gotowego widoku. Wspólny panel służy też dotychczasowym dialogom.
- Zapis 3MF automatycznie dołącza bieżące edycje po zaakceptowaniu nazwy;
  Cancel pozostawia pending baseline. Reload potwierdza utratę zmian. Starsza
  sesja po zmianie sceny zachowuje maski/ustawienia/eksport, zablokowane Apply
  i osobną ochronę przy zamknięciu. Sidebar zachowuje preferencję zwinięcia.
- Runtime GUI RED→GREEN: brak nav, Cancel save czyścił baseline, zagnieżdżone
  menu New/Quit blokowało AT-SPI, Sliced rozwijał zwinięty sidebar.
- Final build0, GUI 5 workflow / 7 audytowanych procesów, 0 prób sieciowych;
  CTest **10/10, 69,84 s**. Oryginalny UVtools: workspace 3 pliki/18 warstw,
  modal CXD 1/6, generator 2/40, native CXD+SL1 3/18 (źródłowy CXD otwierany
  powtórnie). Dokładne piksele, geometria, Z, parametry i materiał PASS.
  Tożsamości źródeł/testów/programu i SHA chronionych wejść zgodne.
- **Następne P0:** archiwa/obrazy/PCB/kalibracje jako dokumenty stałego Sliced;
  pełna macierz zmian profili/zadań, capture cancel/late events/save failures.
  P1: dwa uchwyty zakresu, zoom/scroll/planes, duże maski/DPI/OS. M5/V18 nadal
  IN PROGRESS. Nie zamknięto wszystkich formatów, operacji UVtools, CRUD profili,
  PCB/kalibracji, FFF cleanup ani dystrybucji. Brak nowego pomiaru speedup.
- [Raport](M2_SLICED_WORKSPACE_2026-10-15.md), kanban i manual zaktualizowane.

## Aktualizacja P0 — 2026-10-15, sesja gotowego wyniku i chronione Apply

- SlicedResultSession posiada własne maski, konfigurację, grid Z, placeholdery, statystyki/recipe i deskryptory obiektów bez siatek. Po capture writer context nie zależy od lifetime źródła. Obie ścieżki model/generated edytora korzystają z const owned context; natywne kontenery zachowują własną preserving ścieżkę. Printer preference jest przypisana do drukarki z chwili otwarcia.
- Apply sprawdza source identity/generation przed i pod state_mutex oraz po stop/join. Wrong source/equal revision replacement/cancelled request są odrzucane. Sukces przesuwa baseline i source guard; failure zachowuje dirty/Undo. GUI wykrył utratę Undo po reopen przez użycie export snapshot: coreRED1/1→pełna kopia historii pod source lock→GREEN150/12 i finalGUI PASS. Pierwsze143/11 nie zamykało tej bramki; failed log zachowany.
- Final app/testbuild0, CTest10/10 69.90s PASS. Model GUI brush/range/Undo/Apply/reopen/3MF/restart/CXDLPv4/error-retry2procesy/network0 +independent6records/source preservation PASS. Generated calibration project20layers/restart z innymi presetami2procesy/network0 +originalUVtools2files40records/all fields/source PNG/Z/13params/material PASS.
- Rama tylko odczytana:213warstw, capture sesji22ms, export sesji2,936s, slicing29,152s (jeden przebieg; brak nowego speedup claim). Session/live pliki binarnie identyczne; UVtools3files639records/all published globals/rows zgodne z accepted baseline. SHA modelu i notatek bez zmian, bez zapisania siatki.
- [Raport](M2_SLICED_RESULT_SESSION_2026-10-15.md), manual i kanban zaktualizowane. **Następny etap V18:** rzeczywisty trzeci widok Sliced, lifecycle przy zmianie sceny/drukarki i pełna macierz nonmodal/late-event/cancel. Session owner nie oznacza gotowego workspace ani zamknięcia all-format/UVtools/PCB/calibration/profile/release goal.

## Aktualizacja P0 — 2026-10-15, podział analizy podpór i anulowanie

- Kosztowne fazy przygotowania podpór dzielą zadania do jednej warstwy. Cancellation na wejściu, między fazami/primitives/częściami; callback thread safe. Nie zmieniano geometrii, AA, LCD ani gęstości podpór.
- Aktualna izolowana para na ramce: slicing37,484→28,542s (około24% krócej), preparation21,512→12,556s.452punkty/213warstw. Wszystkie213RLE/raw layer metadata identyczne, tylkoUTCcreationminute różny. OriginalUVtools213records/all exposed fields/pixelSHA zgodne z odebranym baseline. Jeden model i jawne fit/settings; szerszy benchmark pozostaje otwarty.
- RuntimeRED10/18 cancellation→GREEN1477/14, serial vs2/4/16workers exact links/samples/extended shapes/peninsulas PASS. Pierwszy log ma osobno6błędów fixture reference offset pierwszej warstwy; poprawiono. App/testbuild0, CTest10/10 70.10s PASS.
- RealGUI Cancel przy Preparing support analysis0,379s, brak częściowego edytowalnego wyniku, retry zakończony,0prób sieciowych. [Raport](M2_SUPPORT_PREPARATION_SCHEDULING_2026-10-15.md), manual i kanban zaktualizowane. Model i uwagi użytkownika SHA bez zmian; bez kopiowania siatki.
- Nadal otwarte: szersza macierz wydajności/long primitives, full Sliced workspace, configuration/session owner, wszystkie formaty/wersje oraz pełny UVtools/PCB/calibration/profile/release goal.

## Aktualizacja P0 — 2026-10-15, deterministyczna kompozycja SLA odebrana dla ramki

- Odtworzono zależność booleans/area od nieistotnej kolejności konturów i FPU: RED6/36. Stable cyclic rotation/holes/polygon order +scoped Nearest; bez wygładzania, zmiany winding ani wejściowych współrzędnych. Finalcore70/6 PASS, app/testbuild0,CTest10/10 71,69s.
- 3diagnostic +1normal modelrun: wszystkie213composed/raw oraz native records/globals zgodne. OriginalUVtools5largefiles1065records i niezależny SL1→canonicalGray7 PASS. Stary wariant jednego piksela14267→69 zapisany jako jawny delta, nie ukryty tolerance. FinalGUI brush/range/3MF/restart/sidebar NoYesCXDLPv4 +oracle6records/source preservation PASS,network0. [Raport](M2_LAYER_ORDER_DETERMINISM_2026-10-15.md).
- Normal run: slicing39,429s,capture3ms,export7,360s (pojedynczy pomiar, bez deklaracji ogólnego speedup). **Następne P0:** profilowanie native encode/write i bezpieczne cache; native-document owner/full formats/version/cancel/late event matrix oraz reszta pełnego goal nadal aktywne. Broader model/OS determinism matrix pozostaje otwarta.

## Aktualizacja P0 — 2026-10-15, owned conversion i bezpośredni GUI Cancel odebrane w ograniczonym zakresie

- NativeArchiveConversionRequest ma immutable doc/config/format/path; legacy conversion deleguje do tego samego backendu. Guard przez factories/statistics/encoder/readback/parameter transfer i atomic publish. Konwersja nie czyści źródłowego dirty/history. GUI Cancel wiąże się bezpośrednio z requestem także dla native save; cancel przed bind jest pamiętany.
- RED4/18 +worker callbackRED1/3 (28 callbacków poza koordynatorem)→GREEN31/5. Pełna regresja ujawniła retired-reader cancellation przy replace; jawny checkpoint naprawia ten błąd bez restartu starego workera. Focused116/15, final app/testbuild0, CTest10/10 69,87s PASS.
- GUI CTBv4→v5/suffix/source dirtyUndo PASS +independentUVtools exact pixels/all exposed fields/material. Cancel128×8192×4096 i nativeSaveCancel213layers po0,301s, brak output/staging/source loss. ModelGUI brush/3MF/restart/export/failure-retry +independent6records/source preservation PASS;5finalprocesów/network0.
- Frame raster-only export2,929s;213RLE/raw fields identyczne poza creation UTC minute, independent213records PASS. Nie jest to nowy pomiar geometrii. [Raport](M2_NATIVE_CONVERSION_REQUEST_2026-10-15.md). Full format/planes/version/session owner/Sliced workspace/late events i pełny goal nadal otwarte.

## Aktualizacja P0 — 2026-10-15, preserving native save request odebrany w ograniczonym zakresie

- LayerDocument ma source generation niezależne od Undo revision, copy/move/assignment/destruction. NativeArchiveExportRequest przechwytuje immutable document i profil/ścieżkę; const writer publikuje przez wspólny guard/commit. Worker nie mutuje źródła; baseline dopiero po sukcesie i current/same-source. Legacy save również chroniony przed zmianą z callbacku.
- RED6/21→GREEN100/9,app/testbuild0,CTest10/10 73,22s. OriginalUVtools8files24records/SHA/all exposed globals/rows/material PASS dla SL1/CXDLPv4/CTBv4/PM5S. GUI native save +independent6records/rawheader metrics; native Cancel213layers0,301s/dirtyUndo/staging PASS. ModelGUI brush/range/3MF/restart/export/failure-retry +independent6records/source preservation PASS;4procesy/network0.
- [Raport](M2_NATIVE_DOCUMENT_REQUEST_2026-10-15.md), instrukcja w manual.txt. Konwersja, profile/session version owner, native sidebar/full Sliced workspace i pełna queue/cancel/late-event matrix nadal otwarte; pozostały goal bez zmiany zakresu.

## Aktualizacja P0 — 2026-10-15, GUI po błędzie eksportu zachowuje gotowy wynik

- RealGUI RED: po nieudanej atomowej publikacji znikały statystyki. Pierwszą próbę z błędnym selektorem okna zapisano osobno jako błąd fixture. Readiness po stop/join z engine, ponowny check po modal, zachowany finished/Sliced Info; cleanup ExportOngoing i informacja o retry, bez fałszywego sukcesu.
- GREEN appbuild0, GUI error→OK→Edit/Export/stats→retry bez kolejnego Slice PASS; poprzedni target i source PNG/params zachowane, staging usunięty. Dwa procesy/network0. IndependentUVtools6records PASS. CTest10/10 72,18s; końcowy tekst podzielony na krótsze linie, final rebuild/GUI/visual oraz independentUVtools6records/source preservation PASS. Model i notatki SHA bez zmian.
- [Raport](M2_EXPORT_ERROR_READY_UI_2026-10-15.md), instrukcja w manual.txt. Native owner/full formats/error/cancel/source-change/late-events matrix oraz cały goal pozostają otwarte.

## Aktualizacja P0 — 2026-10-15, plan eksportu CXDLPv4 odebrany w ograniczonym zakresie

- RED7/14→GREEN79/1, rozszerzona regresja2379/27 PASS; app/testbuild0. Plan ma callback anulowania na koordynatorze i ograniczony równoległy scan masek. Kolejność float time/material i meaningful bottom zachowana. Test17warstw obejmuje meaningful index8, material0,0391ml, anulowanie i błędny piksel w ostatniej partii.
- Model ramki tylko odczytany: frozen4,293→2,815s (~34%), live4,271→2,833s; plan~1,9→0,465s. Slicing39,558s — nadal otwarty. Binary213RLE/parametry/previews bez zmian poza UTC creation minute. CTest10/10 72,70s; independentUVtools/SL1 PASS213records. RealGUI brush/range/3MF/restart/NoYes PASS,2procesy/network0; independent GUI oracle6records/source preservation PASS. Model i notatki SHA bez zmian.
- [Raport](M2_CXDLPV4_EXPORT_PLAN_2026-10-15.md). Native document owner, pełna macierz cancel/version/late events, export error UX i cały goal nadal otwarte.

## Aktualizacja P0 — 2026-10-15, szybszy readback CXDLPv4 odebrany w ograniczonym zakresie

- Usunięto PNG encode/decode z kontrolnego importu świeżego CXDLPv4; pełna walidacja CRC, parametrów, geometrii i pikseli pozostaje. Porównanie w ograniczonych partiach; user checkpoint tylko na koordynatorze. RED19/25→GREEN1709/16; uszkodzenia staging47/2 PASS, old target/source zachowane.
- Model użytkownika tylko czytany:213warstw, frozen7,348→4,293s, live7,405→4,271s. Slicing38,070→38,099s — ta poprawka go nie przyspiesza. Binary213RLE/pola identyczne poza creation UTC minute; niezależny UVtools +SL1 canonicalGray7 PASS213records. App/testbuild0,CTest10/10 71,95s; realGUI brush/range/3MF/restart/sidebar NoYes PASS,2procesy/network0. Independent GUI oracle6records/source preservation PASS; model i notatki SHA bez zmian.
- [Raport](M2_CXDLPV4_EXPORT_READBACK_2026-10-15.md). Następne P0: planner~1,9s, native owner/queue/error UX/full cancel matrix; dalszy slicing oraz pełny goal pozostają otwarte.

## Aktualizacja P0 — 2026-10-15, usunięty powtórny statistics pass; producer142 otwarty

- Private frozen context korzysta z captured stats, ma guard/checkpoint przed apply/copy/validation; public raster factory nadal liczy pełne statystyki. Native/planes/dimensions/fresh params oraz finite material inputs pozostają walidowane.
- Context0,326→0,00585s, frozen export7,644→7,391s (pojedyncza para, slicing difference nieprzypisywana zmianie). App/test0,CTest10/10 71,44s,realGUI brush/range/3MF/restart/sidebar NoYes/CXDLPv4 +UVtools6records/source preservation PASS,network0. [Raport](M2_FROZEN_CONTEXT_PERFORMANCE_2026-10-15.md).
- **P0 unresolved:** porównanie osobnych slicingów ramki ma różny image SHA warstwy142, wszystkie pozostałe pola identyczne. Liczba/wartości pikseli jeszcze nieustalone. Live i frozen danego przebiegu mają identyczne213RLE/dane druku; dopuszczalnie różnią się timestamp minute (w optimized pair całe SHA identyczne). Problem obrazu jest przed snapshot writer. Failed comparison logs zachowane; nie deklarujemy ogólnej bitowej powtarzalności.
- Następne: diagnostic input/composed/raw + FPU/prep dla142; profilowanie dominujących około7,38s native encode/write. Native owner/full format/version/phase matrix i pozostały pełny goal nadal aktywne.

## Aktualizacja P0 — 2026-10-15, production model queue odebrana w ograniczonym zakresie

- Deferred intent → captured snapshot → preflight/consent → ExportRequest jest używany przez modelowy worker. Own format/path/name, same-source binding, shared cancel przed/po bind, exactly-one bind. RED3/15→GREEN265/25; final374/27 (race/staging). App/testbuild0,CTest10/10; po rozszerzeniu testów SLA393cases PASS, dodatkowy offline API po poprawce menu PASS.
- GUI: File > Export przed pierwszym Slice, suffix/filter switch i SL1 PASS; sidebar brush/range/3MF restart/No zachowujący wynik/Yes→CXDLPv4 PASS. UVtools2files12layers; dla Cx wszystkie expected PNG projection/12params/geometry/identity i source preservation PASS.3audytowaneprocesy/network0. MainFrame CallAfter usuwa blokowanie accessibility podczas native menu chooser. [Raport](M2_DEFERRED_EXPORT_REQUEST_2026-10-15.md).
- **Następna P0:** wydajność/cancel nowego frozen context na dużej ramce. Source evidence: create_raster_job wykonuje raster_job_statistics (validation/decode/area), po czym write_export_snapshot zastępuje te statystyki snapshot; frozen guard/checkpoint jest przypinany dopiero po fabryce. Zmierzyć i usunąć zbędną pracę z zachowaniem pełnej walidacji i responsywnego anulowania. Nowego benchmarku nie wykonano; brak deklaracji przyspieszenia/regresji.
- Native owner, full cancel/version/late events matrix, export error preserving ready status i filename stats po kwantyzacji nadal P0. Pełny goal (all formats/ops/PCB/calibration/profile/3D/release) aktywny.

## Aktualizacja P0 — 2026-10-15, accepted deferred request i production queue w odbiorze

- Deferred intent posiada accepted format/path/name oraz shared cancel przed/po bind; foreign input i external changes/destruction wygaszają wynik. Runtime RED3/15, bind/queue implementation w GREEN build44581.
- Worker ma przechwytywać własny snapshot i używać ExportRequest zamiast live SLAPrint writer. UI No zachowuje gotowe warstwy; printer-ID/SL1 transition/CWS times/palette consent przed write. Nowe real sidebar/initial-deferred GUI scenariusze dodane, odbiór jeszcze wymagany.
- Native document owner, pełna macierz kolejki/cancel/version/late completion events oraz cały goal pozostają otwarte. [Raport](M2_DEFERRED_EXPORT_REQUEST_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, deferred input identity w toku

- Osobny shared input_generation/ExportInputGuard odróżnia przyjęte wejście od powstania nowych masek. RuntimeRED4/21 potwierdził błędne wygaśnięcie przy tym samym modelu.
- Internal raster/automatic refresh/worker project-cache adoption zmieniają tylko output; public config/model/edit/clear/destruction nadal unieważniają input. Pierwszy GREEN207/18; cache RED2/34→GREEN220/20; final core238/22 PASS (metadata refresh, painted masks, callback/no-op, moved-from). App/testbuild0; wszystkie10zestawów CTest PASS po poprawieniu starego testu callbacku (SLA16 998 332/388). GUI brush/range/3MF/restart/CXDLPv4 +UVtools6records/source preservation PASS,network0. Queue wiring wymagane. [Raport](M2_DEFERRED_EXPORT_INPUT_2026-10-15.md).
- Ready/deferred accepted intent, native owner, No/Yes i request stop/reset lifecycle nadal P0. Production queue jeszcze live SLAPrint; cały goal aktywny.

## Aktualizacja P0 — 2026-10-15, frozen request backend odebrany

- Request own/shared const snapshot i format/path/projectname/palette approval, independent cancel flag; cancel/source invalidation/finalrename jeden krótki mutex. Frozen context bez source ModelObject pointers/meshes, copy placeholders/exact levels/stats/recipe i native pixel/vector writer.
- RuntimeRED1/14→finalcore220/20, test/appbuild0,CTest10/10 71,40s PASS. Palette No/Yes/source history, independent cancel, afterstaging cancel/clear, actual hollow/preset/alias i vector→SVG/SL1/CXDLPv4 sprawdzone. UVtools4files21layers PASS. ZwykłaGUI brush/3MF/export2sessions/network0 +independent6records PASS. [Raport](M2_FROZEN_EXPORT_REQUEST_2026-10-15.md).
- **Następna P0:** production queue/sidebar integration (ready/deferred), source input intent odrębny od internal raster output, native owner oraz No/Yes/request lifecycle. Nowy request jeszcze nie wywoływany przez GUI. Pełny all-format/PCB/calibration/profile/release goal nadal aktywny.

## Aktualizacja P0 — 2026-10-15, frozen request w GREEN build

- Request posiada immutable shared snapshot i własne format/path/projectname/palette approval oraz cancellation flag. Cancel i publication ten sam source mutex; request cancel nie zmienia source snapshot guard.
- RED1/14 (valid write stub, cancel/stale/destruction zachowują old target). Frozen writer wdrożony: własny context bez source pointers/meshes, placeholders, exact levels, pixel i vector payload, explicit palette copy. GREENbuild91534 aktywny; runtime/matrix/app/CTest/GUI/UVtools jeszcze wymagane. [Raport](M2_FROZEN_EXPORT_REQUEST_2026-10-15.md).
- Production queue jeszcze nie wywołuje requestu; deferred input intent, native document owner, sidebar No/Yes i lifecycle nadal P0. Pełny goal aktywny.

## Aktualizacja P0 — 2026-10-15, spójny capture backend odebrany

- Engine snapshot zachowuje config/doc/levels/stats/object metadata/recipe/previews; vector-only contours bez borrowed SliceRecords. Dokument bez session history, source generation/lifetime i mutex capture sprawdzone.
- Core1565/16,test/appbuild0,CTest10/10 71,40s PASS. Native Document snapshot2planes/rows/unknownbytes/edited/unedited preserving saves byte-identical. Zwykła GUI brush/3MF/restart/export2sessions/network0 +independent6records PASS. Model i notatki użytkownika niezmienione. [Raport](M2_ENGINE_EXPORT_SNAPSHOT_2026-10-15.md).
- **Następna główna P0:** frozen export context i accepted request z format/version/path/projectname/cancellation owner; integracja kolejki dla engine i native document, deferred input intent oraz sidebar No/Yes. Capture nie jest jeszcze używane przez production scheduler. Pełny refactor/all-format/PCB/calibration/profile/release pozostaje otwarty.

## Aktualizacja P0 — 2026-10-15, engine snapshot w odbiorze

- Spójny immutable capture doc/config/levels/stats/object metadata/recipe/previews pod state_mutex i generation check. Bez mesh pointers i session history; vector-only contours własne.
- RuntimeRED6/14→GREEN1565/16 (model edits/Undo/config/destruction, vector/recipe/cancel/unfinished/threaded). Native Document snapshot zachowuje2planes/rows/unknownbytes i edited/unedited preserving saves byte-identical; native raster conversion guards pozostają.
- Testbuild0,appbuild51581 aktywny. CTest/GUI/readback aktualnych źródeł wymagane. API nie jest jeszcze w schedulerze; frozen request/owner dla engine i native document, deferred slicing i No/Yes nadal P0. [Raport](M2_ENGINE_EXPORT_SNAPSHOT_2026-10-15.md). Pełny goal aktywny.

## Aktualizacja P0 — 2026-10-15, scoped mixed FPU i publication odebrane

- Mixed SSE/x87 RED1/9→GREEN910/16; test/appbuild0,CTest10/10 71,92s. Zapis pełnego fenv i unconditional nearest zachowuje oba stany CGAL/callera.
- **3×213native records/globals** zgodne z baseline, canonicalGray7 identyczne. OsobnySL1 213PNG byte-identical. FinalGUI brush/Undo/3MF/restart/CXDLPv4/source preservation2sessions/network0 +independent6records PASS. Model z Downloads i notatki niezmienione; failed artefakty zachowane. [Raport](M2_LAYER_COMPOSITION_AND_MIXED_FPU_2026-10-15.md).
- Source publication transaction odebrane scoped; następneP0 immutable request (profile/config/planes/rows/previews/provenance), deferred-slicing intent i scheduler cancellation owner. Pełny all-format/PCB/calibration/profile/distribution goal nadal otwarty, szersza geometry/model/OS matrix nieukończona.

## Aktualizacja P0 — 2026-10-15, mixed SSE/x87 poprawka w odbiorze

- Rzeczywisty RED1/9 przy x87nearest/SSEdown→GREEN910/16 po pełnym fenv save/restore i bezwarunkowym nearest. Powiązana factory/renderer/owning/publication/recipe regresja PASS, testbuild0.
- Appbuild31218 aktywny. Powtórzone niezależne ramki/readback, finalCTest i GUI pozostają wymagane. Poprzedni failed pixel142 zachowany; pełna powtarzalność producenta nadal nieodebrana. [Raport](M2_LAYER_COMPOSITION_AND_MIXED_FPU_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, commit odebrany; mixed FPU otwarte

- Scoped source transaction:258/11,test/appbuild0,CTest10/10 71,06s,finalGUI/3MF/rangebrush/CXDLPv4/network0 i independent6records PASS. Recipe/model invalidation przed replacement oraz stale/destruction/blocked publication/rename error cleanup sprawdzone. Full snapshot/scheduler/cancel owner nadal P0.
- Rama47,147s znowu212/213records:pixel142 67→69, wszystkie params/globals identyczne. Failed strict wynik zachowany; nie deklarować globalnej powtarzalności.
- Trace dwóch readonly przebiegów oraz lokalny CGAL FPU.h wskazały lukę mixed SSE/x87. Rzeczywisty rendererRED1/9 (x87nearest,SSEdown) potwierdzony. Guard pełnego fenv/unconditional nearest w GREENbuild65129. Unit/matrix/app/repeatedframe/oracle/GUI jeszcze wymagane. [Raport](M2_LAYER_COMPOSITION_AND_MIXED_FPU_2026-10-15.md). Pełny goal aktywny.

## Aktualizacja P0 — 2026-10-15, atomowy commit w toku

- RuntimeRED4/10 potwierdził, że zmiana/zniszczenie źródła po końcowym check może podmienić poprzedni plik.
- Wdrożono wspólny source state/mutex dla invalidation i krótkiej publication, cleanup staging oraz guard w obu SLAPrint routes. CorrectedGREEN build45745 aktywny; testy runtime/threaded, audyt source paths, app/CTest/GUI/UVtools nadal wymagane. [Raport](M2_EXPORT_ATOMIC_COMMIT_2026-10-15.md).
- Pełny immutable request/capture i cancellation owner pozostają osobnymi obowiązkowymi zadaniami. Karta IN PROGRESS; full goal aktywny.

## Aktualizacja P0 — 2026-10-15, odbiór factory rounding i owning cancellation

- Rounding factory pitch naprawiony wspólnym guardem;24kombinacje orientation/mirrors/gamma×3tryby oraz exception path. Core8314/10,test/appbuild0,CTest10/10 72,31s PASS.
- SL1ramka213PNG byte-identical; CXDLPv4 wszystkie213decodedrecords i globals zgodne z baseline oraz canonicalGray7 osobnego nowegoSL1run. Różnica piksela142 usunięta w opisanej ramce; historyczny failed wynik zachowany. FinalGUI/3MF/rangebrush/export/source preservation2sessions/network0 +independent6records PASS. [Raport](M2_PRINTER_PITCH_ROUNDING_2026-10-15.md).
- Owning document cancellation odebrane dla fabryk/model rasterize. NadalP0: GUI wiring PCB/kalibracji, long primitives, immutable export request/atomic publication i pełna macierz modeli/formatów. Pełny goal nadal otwarty.

## Aktualizacja P0 — 2026-10-15, strict rama i factory rounding

- Owning cancellation: test/appbuild0, core7798/8, CTest10/10 (70,99s), GUI brush/3MF/restart/CXDLPv4/network0 i niezależny6records PASS.
- Strict rama47,102s:212/213masks zgodne, w142 jeden piksel67→69, wszystkie params/globals identyczne. Failed wynik zachowany. Karta REVIEW; nie deklarować pełnej zgodności.
- Nowy factory pitch test odtworzył rounding RED1/13. Guard przeniesiony do wspólnego headera i rozszerzony przed dzieleniem screen/resolution. GREEN build30514 aktywny; app/matrix/GUI i strict kolejnej ramki nadal wymagane. [Raport](M2_PRINTER_PITCH_ROUNDING_2026-10-15.md).

## Aktualizacja P0 — 2026-10-15, owning document cancellation (w odbiorze)

- Wdrożono opcjonalny callback w fabrykach dokumentów; kopiowanie PNG, projekcja palety i walidacja skanują w porcjach, a wiersze parametrów sprawdzają cancellation między rekordami. Rasterize przekazuje swój wyjątek anulowania i przyjmuje nowy dokument po finalcheck.
- Rzeczywisty RED60/100 → GREEN7798/8 wraz z regresją fresh intentions/builder, projected PNG i export snapshot; testbuild0. Appbuild aktywny. CTest oraz końcowy GUI/native readback aktualnego builda nadal wymagane, karta nie jest DONE. [Raport](M2_OWNING_DOCUMENT_CANCEL_2026-10-15.md).
- Pozostają długie pojedyncze decoder/encoder PNG i primitives geometrii, GUI wiring PCB/kalibracji oraz pełny immutable export request/atomic commit. Pełny goal nadal otwarty.

## Aktualizacja P0 — 2026-10-15, GUI Cancel i retry

- Dodano dostępny przycisk Cancel slicing podczas pracy SLA; wspólny mechanizm z ikoną powiadomienia. Edit layers wymaga zakończenia pracy i aktualnego, kompletnego dokumentu.
- Rzeczywisty test GUI: Cancel podczas generowania podpór, powrót do Slice now po 0,411485 s, brak edycji partial result, ponowny slicing i otwarcie edytora PASS. Dwa wcześniejsze błędy GUI odtworzone i poprawione; dowody RED zachowane.
- Build aplikacji0; regresja pędzla2–4/UndoRedo/Apply/3MF/restart/CXDLPv4 PASS, dwa procesy/network0. Niezależny UVtools6records: maski, geometria, Z, parametry i zachowanie oryginalnego projektu PASS. [Raport](M2_GUI_CANCEL_RETRY_2026-10-15.md).
- Nadal P0: owning validation cancellation, długie primitives przygotowania podpór, pełny request eksportu i atomowa publikacja, szersza macierz GUI/modeli/formatów. Odebrany zakres nie zamyka całego refactoru ani pełnego SLA cancellation.

## Aktualizacja P0 — 2026-10-15, cancellation podpór

- Callback głównego generatora dociera do island/peninsula analysis, permanent points, alignment iterations i batch clipping. Wejściowe shapes/permanent/slices zachowane; noncancel point/type coordinates identyczne.
- TDDRED4/8→GREEN461/12,app/testbuild0,CTest10/10 71,38s. Final zwykły GUI/3MF/export2sessions/network0 +independent6records PASS. Rama47,587s,UVtools213/213records pixelSHA/params/meta identyczne.
- Odebrany zakres kernel/generator; realGUI Cancel/latency/stop-join-retry,owning validation i długie support-preparation primitives nadalP0. [Raport](M2_SUPPORT_ALIGNMENT_CANCEL_2026-10-15.md). Pełny goal zachowany.

## Aktualizacja P0 — 2026-10-15, precision przed PNG

- Nowy model slice projektuje paletę przed pierwszym kodowaniem PNG; pełna walidacja owning document pozostaje. Natywny encoder niezależny od capture; cancel/error zachowują poprzedni kompletny cache pair.
- Core897/6,app/testbuild0,CTest10/10 71,55s PASS. CXDLPv4ramka52,248→48,843s; originalUVtools213/213masks/parametry/meta identyczne. SL141,960s,213PNG byte-identical (brak dowodu przyspieszenia Gray8).
- FinalGUIselected2–4/UndoRedo/Apply/3MF/restart/CXDLPv4,2sessions/network0 i independent6records/source preservation PASS. [Raport](M2_PROJECTED_PNG_2026-10-15.md).
- DalejP0: pełny cancel island/peninsula i owning validation, generator migration/native encoding cache, immutable export request/sidebar; pełny goal i macierz pozostałych funkcji nadal otwarte.

## Aktualizacja P0 — 2026-10-15, alignment i powtarzalność AA

- Alignment podpór40,0→6,4s, nadal452points; SL1całość75,1→40,5s i213PNG identyczne po batch clipping. Względem pierwszych108,1s około63% krócej dla opisanej ramki/konfiguracji.
- CXDLPv4całość86,3→52,2s. Rounding problem odtworzony małym testem RED i naprawiony w AGGRaster. OriginalUVtools:213/213masek zgodne z canonicalGray7 PNG osobnego SL1run, wszystkie parametry zachowane.
- App/testbuild0, regression386/11, CTest10/10 PASS. FinalGUI i odbiór aktualnych źródeł opisane w [raporcie](M2_RASTER_ROUNDING_2026-10-15.md).
- Dalej P0: ownedGray7 raster normalization (decode/encode powtórki) i cancellation przez alignment; pełna macierz modeli/gamma/OS/formatów nadal otwarta. Pozostały pełny goal, immutable export request/sidebar/format matrix/PCB/calibration/profile/packaging pozostają w kanbanie.

## Aktualizacja P0 — 2026-10-14, nowe testy użytkownika

- **Odebrane w opisanym zakresie:** jawny pędzel i gumka; nakładanie wybranego zakresu warstw w podglądzie; jeden stroke edytujący cały zakres z atomowym Undo/cancel. Canvas zastąpił statyczny obrazek po rzeczywistym GUI RED. Core44/4, regresja141/8, appbuild0/CTest10/10 i real mouse GUI/Undo/Redo/Apply/3MF/restart/CXDLPv4 PASS (network0); niezależny readback1/6 PASS. Trwały stroke na2–4:134Gray127pixels/3selectedlayers,3unselected intact, Apply/3MF/restart/CXDLPv4 i niezależny readback6records PASS. Planes/zoom/scroll/cancel matrix pozostają otwarte.
- **W trakcie:** diagnostyka wolnego etapu slicing ramki `Bilderrahmen_21(2).3mf` z Downloads, tylko odczyt, bez kopiowania modelu do projektu. Wybrany profil testów: Creality Halot Sky Plus CL-92. Pierwszy etap oznaczony Slicing model57,688s, generowanie punktów podpór40,783s, raster0,968s, SL1export4,151s, całość108,128s. Slicing model obejmuje również przygotowanie podpór/korekty; dokładniejsze pomiary siatki~1,5s — nie przypisywać całych57s przecięciom. Pierwsza optymalizacja: dokładny coverage guard w detekcji półwyspów; SL1całość107,172→73,893s (~31% krócej),213PNG identical. CTest10/10 i finalGUI/oracle PASS. RamkaCXDLPv486,321s,UVtools213records/LCD/mm/Z/12params zgodne,212/213maski zgodne z osobnym SL1run;1AApixel powtarzalności otwarty. Punkty podpór~40s i ownedGray7 normalizacja nadalP0.
- Snapshot eksportowy bez kopiowania Undo/Redo: helper core zaimplementowany, testy history/cancel/current masks PASS (część44/4). Pełny request/atomic commit/sidebar pozostają otwarte.
- Pełny refaktor nadal otwarty; wyniki dotyczą opisanych bramek, nie całej macierzy funkcji/formatów/drukarek.

# Bieżący odbiór — export source generation/lifetime (2026-10-14)

Process-wide IDs rozróżniają źródła także przy equal LayerDocument revision.
Configuration/cache/replace/refresh/clear invalidation i model no-op stability.
Guard trzyma shared atomic state, obsługuje null/move/destruction i anuluje
stale source bez odwołania do zwolnionego SLAPrint. Stop/join pozostaje.

ActualModel runtimeRED4/22, moved-from runtimeSIGSEGV/RED35/2 → final core
GREEN188/9. App/testbuild0, CTest10/10 166,87s PASS. GUI actualModel/edit/
3MF/restart/CXDLPcopy/no/yes/suffix/CRC/source preservation2sessions/network0
PASS. Independent1CXDLPv4/6records canonical7bit source PNG/Z/LCD/12params/
MachineName/MachineZ i wszystkie oryginalne PNG/fresh/settings PASS.
Source/app/test/script SHA stabilne, notatki użytkownika nienaruszone.
[Raport](M2_EXPORT_GENERATION_2026-10-14.md),
[kanban](KANBAN_WDROZENIA_2026-10-11.md).

Zakres guard/publikacja to core z kontrolowanym payloadem i stale przed final
check; nie jest to gotowy native worker request ani atomic check+rename.
Następne P0: [snapshot/commit/request contract](M2_EXPORT_REQUEST_CONTRACT_2026-10-14.md),
bez historii, kompletne dane/profile, capture ownership, threaded stale/cancel/
publication, deferred slicing, następnie sidebar z przyjętym format/version/
copy lifetime i GUI/native readback. Reszta pełnego goal nadal obowiązuje.

# Bieżący odbiór — printer export preflight (2026-10-14)

Shared CXDLPv4 printer identifier contract: przed planem/maskami w
export_print→export_layer_document oraz convert_layer_archive; writer ma
końcową kontrolę. Empty/256byte/NUL odmawiane;255byte/UTF8/legacy sprawdzone.
Edytor odmawia przed chooser/palette i wskazuje istniejące pole Printer >
General > Output > Printer model (firmware), dostępne w Simple.

TDD RED6/24 +conversionRED3/44→focusedGREEN218/10. Final app/testbuild0,
CTest10/10 162,14s PASS. Początkowy CTest9/10 miał fixture repeated-height
z pustym ID; teraz fixture izoluje schedule przez GENERIC i sprawdza także
dokładny komunikat odmowy, bez rozluźniania protokołu.
MissingID GUI2sessions/network0/early refusal/source preservation PASS.
Final positiveGUI2sessions/network0 i independent1CXDLPv4/6records source
canonical7bit PNG/Z/LCD/12params/MachineName/MachineZ/all source preservation
PASS; source/app/test SHA stabilne, notatki użytkownika niezmienione.
[Raport](M2_PRINTER_EXPORT_PREFLIGHT_2026-10-14.md),
[kanban](KANBAN_WDROZENIA_2026-10-11.md).

Następne P0: sidebar z immutable export request/source revision/copy lifetime,
preflight/palette No/Yes/cancel/stale/native readback; osobno profile GUI
correction/save/full CRUD, actualModel range/Z i reszta głównego goal.

# Bieżący odbiór — actual model / 3MF / CXDLPv4 editor (2026-10-14)

Naprawiono worker self-stop i nested state_mutex przy odtwarzaniu warstw.
Core48/1 oraz independent2UVJ/6records/12params PASS; CTest10/10 72,79s PASS
przed późniejszą zmianą GUI. GUI3MF/manual exposure7,12/SL1 named refusal,
no publication/edit preservation2sessions/network0 PASS.

Dodano jawne potwierdzenie eksportowej kopii NativeGray7 dla CXDLPv4.
Build0, core regresja257/5 PASS. Final GUI model/edit/restart/No→absence/
Yes→copy/suffix/CRC/source preservation2sessions/network0 PASS.
Independent1CXDLPv4/6records PNG projected SHA/Z/LCD/12params i wszystkie
oryginalne PNG/fresh descriptor/settings przed/po eksporcie PASS.
[Raport](M2_MODEL_PARAMETER_RESTORE_2026-10-14.md),
[kanban](KANBAN_WDROZENIA_2026-10-11.md).

Następne P0: wczesny preflight i dostęp do poprawy identyfikatora drukarki;
sidebar converted-copy workflow; actualModel range/preset GUI i Z operations.
All-format matrix, complete calib/PCB/operations/performance/release oraz
pełny goal pozostają otwarte. GENERIC to fixture, nie certyfikacja sprzętu.

# Bieżący odbiór — owned parameter table / range GUI (2026-10-14)

Pełna owned tabela walidowana/transakcyjna; sparse changes i jedenUndoRedo,
late-row error zachowuje poprzednie wartości/revision. Zreadonly.
Range parameters stosuje tylko zmienione pola do zakresu first/last.
RED1/1→GREEN124/7, appbuild0/CTest10/10 exit0/72,80s PASS.
GUI range1–4 exposure3,25/wait0,5/save/restart2sessions/network0 PASS.
Independent2UVJ/8records/1project: sourcePNG/Z/LCD/params/union PASS.
[Raport](M2_OWNED_PARAMETER_RANGE_2026-10-14.md).
Otwarte: real3D model/preset/GUI/export matrix, Z/height geometry, native
range normalization/UI,100000-record performance, all native formats i
fullExposureFinder oraz pozostały główny goal.

# Bieżący odbiór — owned per-record parameters (2026-10-14)

Fresh job:14fields read/13editable, Zreadonly. Compact typed history
Entry/FreshRow/FreshOrigin, atomicvalidation/no-op/Undo/Redo, ręczna
zmiana AutoModel→Explicit i rebase po automatic preset refresh wdrożone.
RED1/1 i rebaseRED2/34→focused100213/11 PASS. Appbuild0/CTest10/10
exit0/72,80s PASS. GUI exposure3,25/wait0,5/Apply/Undo/Redo/save/restart,
2sessions/network0 PASS. Independent2UVJ/8records/1project sourcePNG,
Z/LCD/motion/PWM/edited parameters/union PASS.
[Raport](M2_OWNED_PARAMETERS_2026-10-14.md).
Otwarte: actual3D model parameter/preset/GUI matrix, bulk table edits,
Z/height source synchronization, all preserving writers i fullExposureFinder.
Model-origin rebase test jest synthetic kontraktem, nie actualModel GUI.

# Bieżący odbiór — sekwencje naświetlań i union volume (2026-10-14)

Jawny mode w FreshLayerJob/API, project manifest7/descriptor4, prawidłowa
objętość union masek na jednym Z i preserving UVJ wdrożone. Stare strict
guards projektu/czytnika/writera UVJ poprawione. Core100686/12 PASS,
hostile12, FullCTest10/10 exit0/165,38s. GUI labels:4records/2physical layers,
UVJ-only picker, open/edit/Undo/save/restart2sessions/network0 PASS.
Independent core+GUI3files/12records/2projects: PNG SHA, Z, LCD,
exposure/motion/PWM i union0,25mm³ PASS. Final GUI build0 +new CLI1/1 PASS;
core binary niezmienione od pełnego CTest.
[Raport](M2_EXPOSURE_SEQUENCE_2026-10-14.md).
Otwarte: other writer caps/readback, owned per-record parameter editing
z historią/GUI (obecnie fresh rows bez parameter_capabilities), pełny
ExposureFinder recipe/generator/interpretation/profile apply, stationary
PCB i wszystkie pozostałe wymagania głównego celu.

# Bieżący odbiór — wspólny kontrakt pozycji naświetlań (2026-10-14)

Wprowadzono ExposurePositionSchedule: jawne policies, physical groups,
native float/grid boundary preservation i streaming validation bez
dodatkowej tabeli. Production FreshJob/raster factory/project/UVJ używają
wspólnej strict policy. RED3/29→GREEN100900/10, appbuild0, CTest10/10
exit0/172,18s PASS. GUIportrait/restart2sessions/network0 PASS; independent
6files/120layers/5projects strict Z PASS. [Raport](M2_EXPOSURE_POSITIONS_2026-10-14.md).
Repeated mode jest sprawdzony jako kontrakt biblioteki, ale nadal wymaga
jawnego descriptor/schema, union volume, native writer caps i kompletnego
generatora/recipe/GUI ExposureFinder. Nie deklarujemy gotowego eksportu
naświetlań na tym samym Z. Pełny goal pozostaje otwarty.

# Bieżący odbiór — LiftArea taper (2026-10-14)

Generator stopniowo zmniejszanych końcowych masek, wspólny plan dla GUI,
walidacja wysokości/bottom layers i recipe schema2 wdrożone. Runtime
RED22/72→GREEN852/7. Independent UVJ/PWC4files/80layers/4projects,
oba ustawienia orientacji: source PNG SHA, Z, LCD, exposure/motion/PWM PASS.
GUI landscape/portrait,4 sessions/network0, recipe/edit/save/restart/Undo
PASS. Independent GUI4UVJ/80layers/2projects PASS; łącznie8files/160layers/
6projects. Appbuild0 i fullCTest10/10 exit0/217,58s PASS.
Coarse-AA readback test poprawiony: nonzero count może pozostać równy,
sprawdzana jest też ściśle malejąca suma szarości. Produkcyjne maski zgodne
ze źródłem; pierwsze błędne założenie testu zachowane w logach.
[Raport i zależności ExposureFinder](M2_LIFT_TAPER_2026-10-14.md).
Pełna suite dziewięciu kalibracji, interpretacja i profile apply oraz
pozostałe piony głównego goal pozostają otwarte.

# Bieżący odbiór — restart dwóch drukarek (2026-10-14)

GUI3sessions: restartB odtwarza120×70/96×56/PM5Sauto; nativeUp→A i restartA
odtwarza100×60/80×40/PWC517 bez --load/INI edits. Source files retained,
finalselectionA i preferencesA=pwc/B=pm5s/network0 PASS. Independent2/800
+bed/geometry/rawpricing PASS. Permanenttest rozszerzony, bez nowych
zmian produkcyjnych; źródła/app/testSHA zgodne z ostatnim CTest10/10.
[Raport](M2_PRINTER_RESTART_2026-10-14.md).
Otwarte: dirty/normalQuit/popup/mouse/fullCRUD/compatibility, GL/template/
portrait/native matrix/project restart/snapshot i pozostały główny goal.

# Bieżący odbiór — printer combo/LCD lower bound (2026-10-14)

Naprawiono GUImin100 clamping: loaded/saved positive80×40/96×56 są teraz
pokazywane i edytowane zgodnie z core. Same-session native B→A→B wyboru
printer presets odtwarza LCD geometry/PWC517/PM5Sauto518/suffix i osobne
preferences; sourcefiles/network0 PASS. Independent2files/800layers,
INI bed shape=Lcd dimensions, native geometry/rawpricing PASS.
Appbuild0/FullCTest10/10 exit0/77.82s, nowy stały GUI test.
[Raport](M2_PRINTER_COMBO_GEOMETRY_2026-10-14.md).
Otwarte: mouse/restart/fullCRUD, GL visualbed/realtemplates/portrait/upperGUI,
pełne formats/metadata, snapshot/revision i główny goal.

# Bieżący odbiór — material popup mouse (2026-10-14)

Native mouse opens list; outside point potwierdzony geometrycznie anuluje
preview i zachowuje B. Mouse rowA/B aktualizuje fields/Cost, source files/
modelvolume/restartB/network2sessions0 PASS. Independent400layers/rawpricing
PASS; test wierszaB corrected z reviewed5row layout, nie productRED.
Final blankcapture niezaliczony jako visual proof. Permanenttest rozszerzony,
produkcja bez zmian; source/app/testSHA zgodne z ostatnim CTest10/10.
[Raport](M2_MATERIAL_MOUSE_2026-10-14.md).
Arbitrarylists/scroll/disabled/DPI/OS/printer/persistence/native matrices
oraz pozostałe piony całego goal nadal otwarte.

# Bieżący odbiór — material popup keyboard (2026-10-14)

F4 otwiera obserwowany popup; Up/Escape zachowuje B; Up/Enter zatwierdza A,
Down/Enter B, z poprawnymi fields/Cost i source files retained.
GUI2sessions/restartB/network0 i independent400layers/rawpricing PASS.
Permanenttest rozszerzony; bez zmian produkcyjnych, combo/app/testSHA
zgodne z ostatnim CTest10/10 (bez rerun).
[Raport](M2_MATERIAL_POPUP_2026-10-14.md).
Mouse/outside click/disabled controls, printer/LCD/preferences, pełne
profile/native metadata i reszta głównego goal pozostają otwarte.

# Bieżący odbiór — readonly ComboBox focus / material switch (2026-10-14)

Naprawiono readonly widget delegujący focus do ukrytego TextInput.
ActualGUI focusRED→GREEN i Up/Down B→A→B w jednej sesji; pola/koszt/model
volume/pliki presetów/restartB poprawne, network0 dwóch sesji.
Independent400layers/rawpricing, appbuild0 i FullCTest10/10/77.21s PASS.
[Raport](M2_MATERIAL_COMBO_FOCUS_2026-10-14.md). Stały test switch dodany,
wcześniejszy restartA zachowany. Otwarte: mouse/popup/Selection interface,
inne combo i printer/LCD/preferences/fullCRUD/native matrix oraz pełny goal.

# Bieżący odbiór — bottle-weight GUI dependencies (2026-10-14)

Weight1/density0 zachowuje volume1000 bez inf. Weight2/1 przy density2
przelicza volume1000/500 i koszt0.840/1.680; model volume retained.
GUI2sessions/preset-save/draft-source/restart/network0 i independent400layers/
rawpricing PASS. Permanenttest rozszerzony; bez nowych zmian produkcyjnych,
app/testSHA zgodne z ostatnim CTest10/10 (bez rerun).
[Raport](M2_MATERIAL_WEIGHT_GUI_2026-10-14.md).
Otwarte: GUI guidance/range validation, full profiles/combos/metadata,
snapshot/revision, tools/PCB/calibrations/performance/release i cały goal.

# Bieżący odbiór — material preset restart/draft (2026-10-14)

Saved GUI resin price50/volume1000/density0/weight0 restored w drugiej
sesji bez --load/INI edits; positive draft zachował source byte-identical,
selected preset persisted. GUI2sessions/network0 i independent400layers/
rawpricing PASS. Permanenttest rozszerzony, produkcja bez zmian;
Tab/app/testSHA zgodne z poprzednim CTest10/10 (bez nowego rerun).
[Raport](M2_MATERIAL_RESTART_2026-10-14.md).
Otwarte: normalQuit/unsaved prompts, fullCRUD/compatibility/inheritance,
project/model/masks restart, pełne formaty i główny goal.

# Bieżący odbiór — single-price material propagation (2026-10-14)

Naprawiono zmianę samej ceny: primary edit trafia do standardowej ścieżki
update także bez różnicy pola pochodnego. ActualGUI REDcontrol100/
sidebar0.420→GREEN0.840; cykle100/50/0/50, volume retained, wcześniejsze
zero/preset/recovery/filename/PM5S/network0 PASS. Independent400layers/
rawpricing, appbuild0 i FullCTest10/10 exit0/77.71s PASS.
[Raport](M2_MATERIAL_SINGLE_FIELD_2026-10-14.md).
Odebrany jeden workflow, pozostałe controls/combos/preset CRUD/restart,
pełna formatmaterialmatrix i główny goal nadal otwarte.

# Bieżący odbiór — finite resin bottle dependencies (2026-10-14)

Naprawiono GUI density0→volume inf. SLA helper zachowuje bottle volume
przy density i przelicza weight; weight→volume tylko z positive density.
Guards odrzucają nonrepresentable derived fields. GUI0/1000/0 i nowy
SavePreset finite PASS; positive recovery PM5S518/network0 oraz
independent400layers/rawpricing PASS. Core1219/11, appbuild0,
FullCTest10/10 exit0/77.45s PASS. [Raport](M2_RESIN_BOTTLE_2026-10-14.md).
Otwarte: restart/CRUD, single-field propagation, unknown-density weightUX,
pełna materialmetadata/format matrix i reszta głównego goal.

# Bieżący odbiór — material GUI i decimal filename (2026-10-14)

Naprawiono nieaktualny koszt w sidebar po apply oraz utratę dziesiętnej
końcówki masy w nazwie eksportu. ActualGUI RED0.420→GREEN1.680 oraz
RED16→GREEN16.8; volume8.4 retained, sourcepreset unchanged, network0.
Independent PM5S518:400layers/raw materialvolume8.4/weight16.8/cost1.68 PASS.
Core1204/10, appbuild0, FullCTest10/10 exit0/77.67s PASS; permanentGUItest.
[Raport](M2_MATERIAL_GUI_FILENAME_2026-10-14.md).
Odebrany jeden workflow; pełne profile/alias/native format metadata,
snapshot/revision, GUI mutacji podczas export i cały główny goal otwarte.

# Bieżący przyrost — statystyki materiału bez reslice (2026-10-14)

Po zmianie density/cost/bottle volume gotowy dokument natychmiast odświeża
cache masy i kosztu z istniejącej objętości. Zachowane masks/history/time;
bez ponownego dekodowania masek. RuntimeRED4/56→focusedGREEN1199/9,
appbuild0/FullCTest10/10 exit0/77.45s PASS.
[Raport](M2_MATERIAL_STATISTICS_2026-10-14.md).
Odbiór core UVJ model z paint; GUI pricing/density, filename dialog flow,
pełna macierz metadata formatów oraz główny goal pozostają otwarte.

# Bieżący przyrost — raster owner lifetime (2026-10-14)

Naprawiono clear zadania rastrowego bez milestones: jawny stop przed
zwolnieniem masek/writera. Rzeczywisty wątek czyta maski i próbuje export,
callback cancel/join przed clear/replace/apply_scene; cel zachowany.
RuntimeRED5/22→focusedGREEN1583/11. Appbuild0/FullCTest10/10/77.49s,
normalGUI PM5S518/network0 i independent1file/400layers PASS.
[Raport](M2_RASTER_OWNER_LIFETIME_2026-10-14.md).
Otwarte: GUI mutacja podczas kodowania, snapshot/revision/publication,
inne mutatory, real disk-full/process-kill oraz pełny główny refaktor.

# Bieżący przyrost — mutacje danych eksportu (2026-10-14)

SLAPrint zatrzymuje właściciela background przed zmianą konfiguracji
(również identity/pricing/filename) i dokumentu warstw; no-op apply
zachowuje pożyczoną pamięć opcji. RuntimeRED16/25→focusedGREEN1561/10.
Appbuild0/FullCTest10/10 exit0/77.43s; actualGUI normalnego workflow
PWC→PM5S518/network0 i independent1file/400layers PASS.
[Raport](M2_EXPORT_MUTATION_2026-10-14.md).
Otwarte: równoległa zmiana podczas export i GUI Cancel/latency,
pełny snapshot/revision, inne mutatory, real disk-full/process-kill;
pozostałe piony i pełny goal niezamknięte.

# Bieżący przyrost — publikacja eksportu i błędy (2026-10-14)

Wspólny moduł ExportPublication obu API, deterministyczne cancel po write,
wyjątki write/readback, missing/empty, rzeczywista odmowa rename.
Istniejący cel zachowany, brak staging, retry sprawdzony.
Focused608/7 PASS, appbuild0, FullCTest10/10 exit0/77.33s PASS.
[Raport](M2_EXPORT_PUBLICATION_2026-10-14.md).
Dalej P0: snapshot/revision i GUI Cancel/latency; real ENOSPC/permissions,
process-kill/recovery. Pozostałe piony oraz pełny goal nadal otwarte.

# Bieżący przyrost — anulowanie eksportu SLA (2026-10-14)

Oba API wcześniej ignorowały stan anulowania i nadpisywały cel:
runtime RED12/54 dla SL1/CTB/CXDLPv4. Dodano kontrolę wejścia,
callback kodowania i kontrolę przed rename. Focused GREEN498/5,
appbuild0, FullCTest10/10 exit0/77.74s PASS.
[Raport](M2_EXPORT_CANCELLATION_2026-10-14.md),
[kanban](KANBAN_WDROZENIA_2026-10-11.md).
Otwarte P0: mid-write/pre-rename deterministic test, GUI Cancel i latency,
snapshot/revision, disk-full/process-kill. Pozostałe formaty/profile/UI/tools,
PCB/9calibrations/performance/offline release i pełny goal nadal otwarte.

# Bieżący przyrost — jawny format zadania eksportu (2026-10-14)

Draft filename .pm5s/cancel-save/cancel-export preserve sourcepreset/PWCpref
GUI PASS. Accepted format id przekazywany dialog→scheduler jako argument.
ActualexportPM5S518/sourcepreset unchanged/network0 +independent1/400 raw/
reference PASS; appbuild0 iFullCTest10/10 exit0/77.85s PASS.
[Raport](M2_EXPORT_SELECTION_2026-10-14.md). Full snapshot/revision/cancel,
same-session printercombos/failedsave/aliasmatrix ipełny główny goal otwarte.

# Bieżący przyrost — printer preference draft/SaveAs (2026-10-14)

GUIRED source A preference overwrite podczas utworzenia B naprawiony.
Draft format niezmienia trwałego source preference; successfulsave przypisuje
właściwy target. Selector odrzuca niezgodne version/preference.
Core112/3, actualGUI2savedprofiles+restart isolation/network4sessions0,
appbuild0 iFullCTest10/10 exit0/77.68s PASS.
[Raport](M2_PRINTER_PREFERENCE_DRAFT_2026-10-14.md). Same-session combo,
discard/failedsave/suffix/directreexport/fullCRUD ipozostały główny goal otwarte.

# Bieżący odbiór — settings cycles iwait range (2026-10-14)

GUI PWC1/517/auto516→PM5Sauto518→PWCauto516 preserve material/model PASS.
Sharedhelper negative wait diagnostic/panel consistency RED3/45→GREEN861/3;
appbuild0 iFullCTest10/10 exit0/77.20s PASS. [Raport](M2_ANYCUBIC_SETTINGS_CYCLES_2026-10-14.md).
Dwa printerprofiles/restart/preferences, sharedwait GUI correction,
fullCRUD/capabilitymatrix/a11y/DPI iwszystkie pozostałe główne bramki nadal otwarte.

# Bieżący przyrost — Anycubic settings UI (2026-10-14)

Panel Output compatibility pokazuje format/version i nazwę niezgodnej opcji.
Core816/2, actualGUI PWC wait0.125→0 clears warning/model retained/network0,
appbuild0 iFullCTest10/10 exit0/78.15s PASS. [Raport](M2_ANYCUBIC_SETTINGS_UI_2026-10-14.md).
Pozostałe printer/versioncycles, sharedwait/otherfields, a11y/DPI, profileCRUD,
loss contracts iwszystkie pozostałe wymagania głównego celu nadal otwarte.

# Bieżący przyrost — Anycubic modele i early preflight (2026-10-14)

ActualModel19ext/27variants×2axes:756/1 iindependent216files/648layers/
108projects PASS. Early validate unsupported settings/sharedwait RED30→
focused6210/4 PASS; appbuild0. [Raport](M2_ANYCUBIC_MODEL_PREFLIGHT_2026-10-14.md).
FinalFullCTest10/10 exit0/77.11s PASS po scope fix dla raster/import.
ActualGUI preflight, realLCD/profiles/identity, support/
hollowing/drainholes, pełne params/legacy/ZIP iwszystkie pozostałe główne
bramki revision/performance/PCB/calibrations/release nadal otwarte.

# Bieżący przyrost — Anycubic initial producers (2026-10-14)

Version-aware ownership/palette wszystkich19 modern extensions wdrożone.
RED108→finalGREEN6668/6; independent648files/1728layers/162projects PASS.
Actual initialModel DL2P/PM5S core+GUI+independent2/12+4projects PASS;
appbuild0. [Raport](M2_ANYCUBIC_PRODUCERS_2026-10-14.md). FullCTest10/10 exit0/77.09s PASS.
AllModel/GUI variants, realprinterprofiles/bounds/metadata/legacy/ZIP oraz
pełne suites/profileUX/revision/performance/release nadal otwarte.

# Bieżący przyrost — Anycubic modern (2026-10-14)

Owned preserving export19ext/27variants516–518 wdrożony; runtimeRED18→
GREEN1089/1, independent81files/243layers/27projects PASS. ActualGUI DL2P/
PM5S auto version +NoYes loss/source/Undo/network0 iindependent2/12+4projects
PASS. Appbuild0. [Raport](M2_ANYCUBIC_MODERN_2026-10-14.md). FullCTest10/10 exit0/74.73s PASS.
Początkowe ModelSlice/PCB/Calibration dla nowych18ext jeszcze nieadoptowane;
legacy1/515/fullmetadata/printerprofiles/boundaries/GUI/cancel/performance
oraz wszystkie pozostałe wymagania głównego celu nadal otwarte.

# Bieżąca weryfikacja — recipe i eksport PWC (2026-10-14)

30 uszkodzonych projektów odrzucone; focused647/4 PASS. Błędny zapis
zachowuje poprzedni plik i edycje. Nowe exports po recipe persistence:
generated594/1 i independent72files/192layers/18projects PASS.
[Raport](M2_CALIBRATION_RECIPE_VALIDATION_2026-10-14.md).
Pełne matrix/palette/calibration suites, PCB recipe, presets/GUI i
revision/cancel/fault injection oraz pozostały pełny goal nadal otwarte.

# Bieżący etap — przepis kalibracji (2026-10-14)

RuntimeRED potwierdził utratę wzorca/wymiarów/poziomów generatora w projekcie.
Typed recipe, manifest6, capture/restore/validation i read-only GUI review
wdrożone; testbuild0 i core4769/12 PASS. Pełny core16888202/311 PASS. Appbuild0/actualGUI recipe po restart+resave PASS; independent2projects/40layers PASS. Pełniejszy odbiór mask/rows/restart/legacy,
hostile JSON i pełna recipe matrix wymagane. Core311/16888202 PASS;
pozostały CTest9/9 PASS; actualGUI/independent odebrane w opisanym zakresie. Poprzedni odbiór palety pozostaje
punktem bazowym, nie dowodzi nowych zmian. Pełny cel aktywny.

# Bieżący przyrost — kalibracja grayscale (2026-10-14)

Plan requested/effective, duplicate guard i GUI live table/NoYes wdrożone.
RuntimeRED1→GREEN899/6, appbuild0, actualGUI i independent20PNG PASS.
[Raport](M2_CALIBRATION_PALETTE_2026-10-14.md). FullCTest10/10 exit0/74.25s PASS.
Pełne9calibrations/recipe persistence/profile application/innepaletteGUI oraz
pozostałe obowiązkowe zadania głównego celu nadal otwarte.

# Bieżący odbiór — PWC producenci i GUI (2026-10-14)

Actual initialPWC GUI516/518 PASS, independent2files/12layers/4sourceProjects
PASS; source unchanged/Undo/network0. PWC CalibrationXYZ/grayscale/PCB
516/517/518×2axes: core594/1, independent72files/192layers/18projects PASS.
[Raport](M2_PWC_GENERATED_AND_GUI_2026-10-14.md). Nowy pełny CTest10/10 exit0/73.55s PASS.
Nowe P0: jawne requested/effective grayscale i wykrycie zduplikowanych
poziomów po kwantyzacji (GUI domyślnie32, PWCnative34).
Wciąż otwarte: GUI generatorów i517, pełne calibration/PCB suite, parametry/
metadata/boundaries, legacy1/515, loss UX, revision/cancel/performance oraz
pozostałe wymagania głównego celu. Niższe wpisy są historią checkpointów.

# Bieżący przyrost — początkowy producent PWC (2026-10-14)

Model z początkowym PWC auto/516/517/518 otrzymuje owned job i NativeGray4.
RuntimeRED2 → GREEN764; build0. Independent raw + UVtools FullOpen:
10 plików/30 warstw/2 projekty, zero różnic. [Raport](M2_PWC_INITIAL_PRODUCER_2026-10-14.md).
Test wyboru wersji i wide10871857/177 PASS. Appbuild0 i pełny CTest10/10 (73.40s) PASS. ActualGUI, pełne metadata,
PCB/kalibracje, legacy1/515, cancel/performance wymagają dalszego odbioru.
Pełny cel nadal otwarty; aktualne priorytety w [kanbanie](KANBAN_WDROZENIA_2026-10-11.md).

# Bieżący odbiór — SL1 transitions / pełny CTest (2026-10-14)

FullCTest10/10 PASS74.03s. ActualSL1GUI No/Yes/export/source project/Undo/
network0 oraz independent1file/6layers+2sourceProjects PASS; app71213424….
Source exposure doubles zachowane, native schedule20/14.17/8.34/2.5… zgodny
z reference. [Raport](M2_SL1_TRANSITION_PROJECTION_2026-10-14.md).
Pełny goal niezamknięty: all formats/variants/operations, source origins,
SL1S/range/tie/clipping, direct/sidebar/CLI loss UX, result/revision/cancel,
profiles/Sliced/PCB/calibrations/performance/offline distribution obowiązkowe.

# Bieżący przyrost — UVJ GUI / SL1 native transition (2026-10-14)

UVJ actualGUI +independent1/6+2sourceProjects PASS. SL1 canonical curve
projection zachowuje source i odmowę arbitrary override: focused3400/2,
wide10871731/176, appbuild0 PASS. [Raport](M2_SL1_TRANSITION_PROJECTION_2026-10-14.md).
FullCTest w toku; independentnewSL1curve i GUI No/Yes, ranges/ties/clipping
oraz cały pozostały goal nadal wymagane.

# Bieżący przyrost — UVJ GUI / SL1 transitions (2026-10-14)

Actual UVJ GUI list/suffix/export/source snapshots/Undo/network0 PASS;
independent1file/6layers+2projects PASS. SL1 canonical source transition
runtimeRED1→focusedGREEN3400/2 (wraz z wcześniejszym CWS cross-export test).
Arbitrary override nadal refuse. Native exposure schedule projection report
+GUI confirmation added; wide/appbuild w toku, independentSL1/GUI/fullCTest
jeszcze wymagane. Full goal niezamknięty.

# Bieżący przyrost — UVJ owned final (2026-10-14)

Typed fresh plan/writer/publication, range/phase/grid guards oraz helper9codec
wdrożone. Final774/5, independent6files/18layers+2projects, wide10871727/175
PASS. Actual initialModel UVJ oraz Calibration migration ownership verified.
Appbuild0/SHAd43e04ca…; [raport](M2_UVJ_OWNED_2026-10-14.md). ActualGUI,
fullCTest, rich metadata getters/counters/price/time, full Calibration/PCB
exports i reszta formatów pozostają otwarte. SL1fractional schedule P0.

# Bieżący przyrost — UVJ owned (2026-10-14)

Typed source/nativefloat plan +actual UVJ JSON/PNG writer i publication proof
wdrożone. RuntimeRED2→focusedGREEN50/2; independent6files/18layers+2projects
PASS obu osi. Range/phase/grid refusal preserves sentinel/source. Initial
producer TDD/model ownership w build5915; app/GUI i native metadata broader
matrix nieodebrane. FullCTest8/10 poCWS (SL1/UVJ failures); SL1 fractional
schedule oraz cały refactor nadal otwarte.

# Bieżący przyrost — CWS time projection (2026-10-14)

Source doubles +represented uint32ms wdrożone, original project zachowany.
Focused1073/6, independent6files/18layers+2projects, wide10871601/173 PASS.
Appbuild0/SHA22abf21b…; actualGUI No/Yes/source project/Undo/network0 i
independent1file/6layers+2projects PASS. [Raport](M2_CWS_TIME_PROJECTION_2026-10-14.md).
Głębsze fresh→UVJ i fractional→SL1 P0 ujawnione; FullCTest, ranges/ties,
sidebar/direct/CLI projection UX i pełny goal nadal otwarte.

# Bieżący przyrost — CWS independent i time plan (2026-10-14)

Independent long schedule6files/6000layers+2projects PASS. Source/represented
millisecond plan wdrożony; source doubles preserved, canonical codec strict.
Focused1073/6 oraz independent6files/18layers+2projects PASS; GUI explicit
summary/No/Yes added, lecz nowy app/GUI nieodebrany. Underflow positive-time
refusal/preserve tests w build54874. Głębsze CWS→SL1 fractional schedule i
UVJfresh unsupported P0 ujawnione; pełny CTest nadal otwarty.

# Bieżący przyrost — CWS Z guard (2026-10-14)

Long-schedule RED2/8→focusedGREEN1045/5; exact vectors/trajectory i one-ULP
mutation rejection retained. Archive178/1, wide10871573/172 PASS.
Appbuild0/SHA7bb40ace…; encoder bytes unchanged i source identity PASS.
Independent long-job/readback/GUI oraz fractional transition exposure/native
millisecond projection i fullCTest nadal wymagane. [Raport](M2_CWS_Z_ACCUMULATION_2026-10-14.md).

# Bieżący przyrost — CWS Z accumulation (2026-10-14)

Pure1000rows ×2actual source positions odtwarza layer21: RED2/8 →
focusedGREEN1045/5. Exact command vectors/trajectory i one-ULP tamper
rejection zachowane; source forward-error bound oparty o konkretne operacje,
native grid0.001 nadal wymagany. Encoder bytes bez zmian. Archive test
przechodzi CWS i ujawnia CX4missing-model w shared fixture; helper poprawiony,
build w toku. Full archive/independent/app/CTest jeszcze nieodebrane.
CWS fractional exposure/native projection nadal P0.

# Wynik ponowienia CTest (2026-10-14)

CLI PASS. Core301cases/2failedcases i reference91cases/1failedcase nadal RED
wyłącznie CWS: fractional transition exposure oraz layer21 executable
schedule mismatch (do izolowanego odtworzenia i sprawdzenia akumulacji Z).
CXidentifier failures usunięte przez poprawne inputs; guard zachowany.
PWC GUI-copy516/518 i independent readback odebrane w opisanym zakresie.
FullCTest/fullgoal nadal otwarte.

# Bieżący przyrost — CX fixtures i PWC GUI (2026-10-14)

PWC GUI516/518 + independent2/12+4sourceProjects PASS. CX jawny model
w synthetic fixtures poprawiony; CLI26checks oraz native112/2 PASS.
Negative missing-model guard obu exports zachowuje sentinel/source.
Trzy wcześniej failed CTest gates ponawiane, wynik w toku (sesja34089).
CWS transition-time/native projection nadal P0; full goal niezamknięty.
[Raport](M2_MODEL_APP_ACCEPTANCE_2026-10-14.md).

# Bieżący przyrost — PWC export-copy GUI (2026-10-14)

Actual GUI516/518 PASS: explicit loss summary No/Yes, poprawny suffix/writer,
source project PNG/params/Z unchanged, Undo available, network0attempts.
Independent2files/12layers+4sourceProjects PASS. Appbuild0/SHA7814aafd…;
[raport](M2_PWC_PALETTE_2026-10-14.md). Initialproducer/import/sidebar/fullGUI
nadal otwarte. CXmodel fixtures poprawione (software-only), testbuild w toku.
CWS transition-time i pełny CTest pozostają nieodebrane.

# Bieżący przyrost — PWC palette core/API (2026-10-14)

NativeGray4 source/editor/persistence PASS551/1. Explicit export copy,
loss summary/cancellation/source preservation focusedPASS866/4;
independent18files/54layers+12projects PASS obu osi i516/517/518.
Wide10871555/171 PASS (filtr, nie fullCTest). Appbuild90684 trwa;
GUI oraz initial producer adoption pending. [Raport](M2_PWC_PALETTE_2026-10-14.md).
CWS transition time i CXidentity nadal P0; full goal pozostaje otwarty.

# Bieżące wdrożenie — PWC palette (2026-10-14)

NativeGray4 i project/editor contract oraz explicit palette export-copy API
wdrożone. RuntimeRED241/513 przed zmianą; build GREEN jeszcze w toku.
GUI ma analizę zmian, wybór No/Yes i zachowanie source/dirty/history,
ale code/appbuild/actualGUI/cancellation/export readback wymagają odbioru.
CWS transition time i CXmodel cards z fullCTest nadal otwarte.

# Bieżący odbiór aplikacji — RED (2026-10-14)

Nowy appbuild PASS (SHA4bf0d0d…), source identity unchanged. Pełny CTest
**7/10**, 3 failed gates: wymagany CXprintermodel w fixtures/CLI oraz CWS
transition exposure poza exact integer-ms. Actual GUI PWC516 eksport
odrzucony dla Gray8: brakuje jawnego loss-choice/palette projection;
wersja518 nieuruchomiona. Nowe P0 cards w kanbanie. [Raport](M2_MODEL_APP_ACCEPTANCE_2026-10-14.md).
Zielone wyniki wcześniejszego filtra nie zastępują pełnego odbioru.

# Bieżący przyrost — PWC Z/publication (2026-10-14)

Preflight zapobiega zlaniu pozycji Z na publicznym grid0.001 i nadpisaniu
istniejącego pliku. RuntimeRED15/27→focusedGREEN483/3; independent9/27
+3projects PASS; wide10870842/169 PASS. Header/EXTRA globals i konkretna
wersja sprawdzane przed publication. Nowy appbuild rozpoczęty, wynik/GUI
jeszcze nieodebrane. Full decimal/float/material/time/bbox boundaries,
producenci/paleta/pozostałe variants nadal otwarte. [Raport](M2_PWC_FRESH_2026-10-14.md).

# Bieżący przyrost — PWC fresh (2026-10-14)

Plan źródłowych parametrów, writer516–518 i publication readback wdrożone.
Speed precision runtimeRED18 → focusedGREEN456/2; independent9files/27layers
+3projects PASS. Regresja7334/7, wide10870815/168 PASS.
[Raport](M2_PWC_FRESH_2026-10-14.md). PWC nadal IN PROGRESS:
Z/float boundaries, global/time/material proof, native source palette,
pozostałe warianty/producenci i appbuild/GUI wymagają odbioru.
Pełny goal pozostaje otwarty; historyczne wpisy niżej opisują swoje checkpointy.

# Bieżący przyrost — CBDDLP owned writer (2026-10-14)

CBDDLP/Photon v2 typed writer i atomic both-route publication podłączone.
Build0, focused4982/2, independent72files/216layers +24projects PASS.
[Raport](M2_CBDDLP_WRITER_2026-10-14.md). Wide10870675/167 ma1fail PWC516.
CBD fullnegative/allAA/initial model+calibration/PCB/native params/GUI/variants
pozostają obowiązkowe; bez nowego appbuild/fullCTest/hardware odbioru.

# Bieżący przyrost — CBDDLP palettes (2026-10-14)

Source AA1–16 palette/editor/restart/native counts encoder zweryfikowane:
GREEN8400/1, FullOpen/raw16files/48layers +16projects PASS.
[Raport](M2_CBDDLP_PALETTES_2026-10-14.md). Fresh metadata exporter nadal
IN PROGRESS; native pliki tego gate używają legacy context, nie neutral
parameter publication. Wide10865738/166:2fail CBDDLP/PWC nadal P0.
Następne: typed CBD writer/pricing/time/source row proof, oba exports i
independent roundtrip. GUI/native params/dose policy/appbuild/fullgoal otwarte.

# Bieżący przyrost — CBDDLP preflight / CWS height (2026-10-13)

CBDDLP IN PROGRESS: pure native v2 builder runtimeRED1→GREEN50/1. Writer,
AA/masks/publication/pricing/time/native readback jeszcze niepodłączone.
CWS source-Z guard naprawiony: runtimeRED2/3 (nadpisany sentinel poza
printer height) → reject/preserve PASS. Focused1682/7; wide70642/164 ma
2fail CBDDLP/PWC nadal P0. [Raport](M2_CBDDLP_PREFLIGHT_2026-10-13.md).
Nie ma nowego appbuild/GUI/fullCTest ani odbioru CBD native plików.

# Bieżący przyrost — initial CWS / legacy (2026-10-13)

ModelSlice z CWS wybranym od początku ma teraz własne parametry; helper8codec.
Legacy4dialects no-lift/dark timers oraz XMLsync naprawione. Build0,
focused1629/5 i independent24files/72layers PASS. [Raport](M2_CWS_PRODUCER_LEGACY_2026-10-13.md).
Wide70589/162:2fail/1case CBDDLP/PWC516 nadal P0. CWS generator-specific
Calibration/PCB, owned inne dialects, native params reader/editor i GUI
pozostają do odbioru. Full3mf/appbuild/fullCTest/release także nieodebrane.

# Bieżący przyrost — CWS boundaries (2026-10-13)

Owned CWS active no-lift exposure naprawione: independentRED12→GREEN24files/
72layers. Bez dopisywania fizycznych ruchów/czasów; prelude reset tego samego
indeksu i strict app reader. Build0, focused831/3 PASS; regression70309/161
2fail CBDDLP/PWC516 nadal otwarte. [Raport](M2_CWS_BOUNDARIES_2026-10-13.md).
Następna P0: **CWS legacy/initial producer** — siedmiocodecowy migration helper
nie obejmuje jeszcze CWS; owned poprawka nie jest dowodem tej ścieżki.
GUI/native editable params/dialects/pełna matrix i nowy appbuild nadal pending.

# Bieżący przyrost — CWS preserving (2026-10-13)

M2.4b.2b.2-CWS **IN PROGRESS**: typed commands/reader/plan i export wiring
wdrożone. Build0, focused315/2; FullOpen/raw6files/18layers PASS.
Pełna boundary matrix, native import/editable parameters, dialects i GUI
projection disclosure pozostają otwarte.
Regresja69793/160:2fail/1case — ujawnione za wcześniejszym CWS błędem
**CBDDLP i PWC516 fresh preserving exporters**. Nowe P0 cards w
[kanbanie](KANBAN_WDROZENIA_2026-10-11.md). Nie downgradowano source ownership.
Nowy appbuild/GUI/fullCTest jeszcze nieodebrany; pełny goal pozostaje aktywny.

# Bieżący przyrost — precyzja masek (2026-10-13)

M2.4b.2b.2-mask **IN PROGRESS**: source Gray8/NativeGray7, świadoma projekcja
przy generowaniu, edycja przed commit, strict restore i manifest5/descriptor3
wdrożone. Build0; focused2226/3 PASS; regression69194/158:1fail CWS. Independent
generated112/280+28projects,0errors. Brak odbioru nowego app/GUI/fullmodel matrix.
M2.4b.2b.2-CWS **READY P0**: plan preserving writer/parser ukończony, kod nadal
do wdrożenia. [Kanban](KANBAN_WDROZENIA_2026-10-11.md),
[raport bieżący](M2_MODEL_OWNERSHIP_2026-10-13.md).

# Aktualna praca — ModelSlice (2026-10-13)

M2.4b.2b.2 IN PROGRESS: automatic vs explicit parameter origin oraz real-model
owned generation/lifecycle/persistence wdrożone; focused518/1 PASS. Corrected
regression67487/157 nadal ma2fail: CXDLPv4 fresh mask/editor precision i
SL1owned→CWS missing preserving contract. Oba P0 blokują odbiór modeli.
Nowy appbinary/GUI/fullCTEST i independent actual-model exports nie są odebrane.
Ostatnie zweryfikowane binarium pozostaje checkpointem Calibration/PCB poniżej.
[Raport](M2_MODEL_OWNERSHIP_2026-10-13.md), [kanban](KANBAN_WDROZENIA_2026-10-11.md).

# Bieżący checkpoint — 2026-10-13

**M2.4b.3-ui DONE — statystyki/presentation/capture:** wdrożono prezentację statystyk własnego zadania, poprawne źródło kosztu, bezpieczny czas double i naprawę capture source Z dla model/generated. Focused580/15 i regression65028/134 PASS. Rzeczywiste3sesje GUI,6hover screenshots i CTest9/9 PASS. Poprawiono również przycinanie dużych etykiet suwaka; final app build0 i zgodność źródeł/binarium potwierdzona. [Raport](M2_STATISTICS_UI_2026-10-13.md), [bieżący kanban](KANBAN_WDROZENIA_2026-10-11.md). Historyczne sekcje poniżej nie zastępują bieżącego statusu.

**M2.4b.2b.1 DONE — Calibration/PCB core/API dla7codec:** własne parametry po finalnych overrides, sourceDOUBLEZ, stationary PCB i zachowanie projektu/restaru. Native CTB/AES material precision naprawione: strict RED80→GREEN0. Focused2762/6, regression68188/156, CTest9/9, fullbuild0; independent112files/280layers+28projects i13identitySHA PASS. Actual GUI regression statystyk3sessions/6hover images PASS. [Raport](M2_GENERATED_OWNERSHIP_2026-10-13.md). **Następna P0:** ModelSlice provenance i reslice lifecycle. Parent producer wiring, universal formats, pełny GUI/Sliced, profiles, PCB/calibrations suite, performance/cancel i release offline pozostają otwarte.

# Status TODO — aktualizacja wdrożenia 2026-10-13

**M2.4b.3 — odebrany zakres owned raster statistics core/API:** rzeczywiste masks + własne DOUBLE geometry/Z/rows/counts zamiast unrelated preset; neutralny czas bez codec setup/rounding jawnie oznaczony w API. Finite material/time guards i atomowa odmowa replacement, edit/UndoRedo/save/restart/portrait/producers oraz plain-PNG fallback. Focused489/8, regression64937/127, app/testbuild0, CTest9/9 (56.53s), source5+binaria2 snapshot. [Raport](M2_OWNED_STATISTICS_2026-10-13.md), [kanban](KANBAN_WDROZENIA_2026-10-11.md). **Następne:** GUI koszt/area/time/large-value formatter, pełny background model restore i skrajne zakresy arytmetyki; producer wiring, all-format contracts, performance/cancel oraz fullgoal nadal otwarte.

**M2.4b.5f — odebrany owned fresh SL1/SL1S core/API:** typed globals/schedule/firstZ i notesLightOff zamiast destination preset; exactPNG, counts, time i physicalvolume. Named refuse dla niewykonalnych rows/nativeprecision, finite profile guard i ochrona poprzedniego pliku. RED72/184 +materialRED24/328→GREEN2348/4, regression64448/119, CTest9/9 (49.34s), linked app/test build0. Independent92files/276layers +64projects/11SHA PASS. [Raport](M2_SL1_FRESH_2026-10-13.md), [kanban](KANBAN_WDROZENIA_2026-10-11.md). **Otwarte:** owned statistics/producer/GUI/history, all-format contracts, cancel/performance i duże counts/Z/names oraz real hardware. To odbiór core/API, nie ukończenie pełnej aplikacji.

**M2.4b.5e — odebrany owned fresh CXDLPv4 core/API:** typed plan/writer obu export routes zachowuje jawne globals/rows/geometry/Z i pixels, przed publikacją sprawdza native/effective parametry, time/volume/weight/cost. Named refusal chroni poprzedni plik, finite material overflow naprawiony RED8/8→GREEN. Final1612/5, regression62100/115, CTest9/9 (48.77s), linked app/testbuild0. Independent36files/108layers +32projects,9SHA PASS. [Raport](M2_CXDLPV4_FRESH_2026-10-13.md), [kanban](KANBAN_WDROZENIA_2026-10-11.md). **Kolejna P0:** fresh SL1 contract5f. Producer/statistics/GUI/nativeediting, cancel/performance i all-format/fullgoal pozostają otwarte.

**M2.4b.4a.1 — odebrana korekta generated CXDLPv4 time:** przygotowanie300s, per-row motor rounding i zapisuint policzone z tych samych finalnych wartości co native table. RED8/648→focused1300/2; regression55941/102,CTest9/9 (47.84s),build PASS. Independent16files/48layers time/phase/geometry/Z/pixels PASS, integerresidual<1s. [Raport](M2_CXDLPV4_TIME_2026-10-13.md). **Otwarte:** tiny-speed global fallback/precision policy, owned fresh/imported CX contract, GUI/statistics i fullgoal.

**M2.4b.4 — odebrana korekta ruchu CXDLPv4:** per-layer TOTAL/FIRST naprawione; independent8files/24layers raw/public motion, geometry/Z/pixels PASS. Focused296/1, finalregression54937/101, CTest9/9 (66.64s), app build PASS. [Raport](M2_CXDLPV4_TWO_STAGE_2026-10-13.md). **Ten historyczny time P0 naprawiono w M2.4b.4a.1; precision/fallback i full metadata nadal otwarte.** Nie uznajemy jeszcze pełnej poprawności metadata/fresh export tej rodziny.

**M2.4b.2a — odebrany pure builder API:** typeddoubles, engine actualZ, oriented geometry, pełne normal/bottom defaults i oryginalny transition denominator; 144warianty, focused6913/2, regression53724/99, CTest9/9 (84.90s), linked app PASS. [Raport](M2_FRESH_BUILDER_2026-10-13.md). Integracja producentów/GUI/statystyk pozostaje otwarta. Następne P0: CXDLPv4 per-layer TOTAL/FIRST znalezione w przeglądzie oraz preserving contracts SL1/CXDLP przed wiring.

**M2.4b.1 — odebrany eksport fresh w core/API:** obie publiczne ścieżki zachowują owned globals/rows/geometry dla CTB4/5/AES4/5. SettingsRED128/416 oraz namedRED8/8 → focused1704/2 PASS; regression46811/97, CTest9/9 (48.28s), independent72files/208layers i32projects/96rows/96masks PASS. Float32 dotyczy planu, project doubles pozostają dokładne. [Raport](M2_FRESH_EXPORT_2026-10-11.md). M2.4b.2a pure builder odebrany; producentów/statystyk i fresh GUI nadal nie podłączono. Pełny goal aktywny; SL1/CXDLP oraz pozostałe fresh contracts wymagają dalszego wdrożenia przed globalnym wiring.

**Odebrana M2.4a — fundament core/project:** jawny neutralny descriptor świeżych zadań i zapis projektu v3; plain PNG nadal bez parametrów. Focused708/1, regression45107/95, CTest9/9 (79.84s), linked app build PASS. Independent4producers/8archives/24rows+24masks: dokładne doubles/Z/geometry oraz wszystkie JSON/PNG payload bytes po drugim zapisie PASS. [Raport](M2_FRESH_FACTORY_2026-10-11.md). Następna [M2.4b](M2_FRESH_PRODUCER_INTEGRATION_2026-10-11.md) wymaga preserving/reject gates eksportu przed podłączeniem producentów, potem statystyk i GUI. Pełny goal pozostaje aktywny; to nie jest gotowy fresh GUI/export workflow.


**Odebrany M2.3b.4c — numeric writer / AES table / timing:** poprawione numeric identity/timing i prywatny codec transfer pełnej tabeli nowego AES destination. RED164/560 +additionaloverflow32/2096 → final2480/2 i regression16562/57; CTest9/9, independent388files/1164layers/192pairs+4fresh i actualGUI2 PASS. RAWpreservation i publicrounding/fallback odróżnione, strictfailedreports zachowane. [Raport](M2_NUMERIC_TILT_2026-10-11.md). Dodatkowy independent Any516/51724pairs/48files/144layers PASS, native352/1. Razem finalstage436paths/1308layers/216pairs+4fresh; całego goal/freshUI/pełnego format matrix nie zamyka ta karta.

**Odebrany M2.3b.4b — named AES tilt:** writer zapisuje stałe pola ruchu i czas4s dla Saturn4Ultra/Mars5Ultra; konwersja sprawdza globals/rows przed encodingiem. RED116/184 → native14082/55, CTest9/9 (52.59s), build0; independent12files/36layers i6duplicatev5names PASS. ActualGUI2success/refusal z target/source/dirty/UndoRedo zachowane, FullOpen2/6/raw1/3 PASS. [Raport](M2_NAMED_TILT_TARGET_2026-10-11.md). Numeric bootstrap/timing osobna P0 M2.3b.4c; pełny identity, effective fresh table/UI oraz wszystkie formaty nadal otwarte.

**Odebrany M2.3b.4a — source classifier i GUI lock/unlock:** numeric classifier uchylnej kuwety AES używał błędnych pól (bottom speed zamiast first lift i normal speed). RED36/62, poprawka i14wariantów kodu/testów wdrożone; final build0, native13794/54 i independent14/42 GREEN; CTest9/9 i actualGUI2readonly/editable+nativeSave PASS, dodatkowy FullOpen4/12 i raw14fields/pixels bez różnic. [Raport](M2_PRINTER_MOTION_2026-10-11.md). To nie zamyka writer policy dla tilt ani target printer identity.

**Odebrany zakres M2.3b.3 — znana natywna skala:** źródłowa fizyczna geometria CTB/AES/Any, obie oriented osie porównane w natywnej precyzji i actual Z przed kodowaniem. RED120/320, z czego80fail dotyczą cichej zmiany skali, a40 wcześniejszego/konkretnego błędu (końcowe bramki chroniły resolution/Z). Final build0, native13648/53, independent360files/1080layers GREEN. CTest9/9 i GUI2+widthmismatch1 PASS; dodatkowy FullOpen5/15 i raw2pary bez różnic. [Raport](M2_CONVERSION_GEOMETRY_2026-10-11.md). Unknown descriptors, mirror/identity i jawny rescale/loss-report UX nadal otwarte.

**Odebrany strukturalny zakres M2.3b.2:** wspólny owned preflight parametrów, wskazanie nazwy/jednostki traconego pola oraz walidacja globals/Z przed kopiowaniem masek. RED4 → native12991/51, CTest9/9 i FullOpen240/720 GREEN. Build PASS; GUI2 konwersje+odmowa1+absence1 PASS, FullOpen4/12/raw2pary bez różnic. [Raport](M2_CONVERSION_PREFLIGHT_2026-10-11.md). Pełny capabilities/loss i GUI wyjaśniające odfiltrowane pozycje pozostają otwarte. Nie zamyka geometrii mm, printer identity ani pełnej macierzy formatów.

**Odebrana podkarta M2.3b.1; pełny M2.3b nadal w toku:** [rozdział neutralnego modelu i ograniczeń CTB](M2_NEUTRAL_PARAMETER_CONTRACT_2026-10-11.md). Runtime RED4/4 → focused3562/5, regression12406/50 i CTest9/9 GREEN. Kod rozdzielony; FullOpen nowych fixture240/720 i120 par bez różnic. GUI2 konwersje+odmowa invalid_height1 PASS, dodatkowy FullOpen4/12 i raw2pary bez różnic. Undo/Redo/target/source zachowane. Wspólny capabilities/loss preflight pozostaje otwarty. Fresh tables i ich GUI pozostają otwarte.

**Aktualna partia — M2.3a odebrana w zakresie globals CTB4/5/AES4/5:** [raport](M2_CTB_GLOBAL_CONVERSION_2026-10-11.md). Naprawiono podmianę globals przez preset, zachowano natywną precyzję i dodano odmowy niejednoznacznych flag/pól. Native12300/48, CTest9/9, FullOpen240/720, raw globals120 par i mutation20 PASS; GUI2+odmowa1 oraz dodatkowe FullOpen4/12/raw2 PASS. [Kanban](KANBAN_WDROZENIA_2026-10-11.md) nadal wskazuje otwarte M2.3b–M8 i pełny goal. [Fresh tables — projekt kolejnej karty](M2_FRESH_TABLE_DESIGN_2026-10-11.md) nie są jeszcze wdrożone.

**Bieżąca partia M1/M2:** [raport wdrożenia](WDROZENIE_M1_M2_2026-10-11.md). Globalne ustawienia Any516/517→CTB/AES odebrane: native10556/47, CTest9/9, FullOpen96/288 + independent48/144 i GUI2 bez różnic. Walidator LCD/px/Z i kreator odebrane w tym zakresie; AT-SPI i mysz PASS, CTest po UI fix9/9 (59.39 s). Pełne M2–M8 i główny goal pozostają otwarte.

**Historyczny checkpoint M1 przed M2; wdrożenie zatwierdzone 2026-10-11.** [Bieżący kanban](KANBAN_WDROZENIA_2026-10-11.md) zastępuje historyczne oczekiwanie na zgodę. [M1: tabela Anycubic→CTB/AES](ANYCUBIC_CTB_CONVERSION_2026-10-11.md): native10021/42, CTest9/9, niezależny raw/UVtools i GUI PASS. Konwersja pozostaje częściowa: globalne nagłówki wymagały poprawki M2, odebranej w bieżącej partii opisanej wyżej; pełny goal otwarty.

**Historyczny audyt przed zgodą i implementacją, 2026-10-11:** [pakiet A–J, piony V01–V27, roadmapa i TDD](AUDYT_I_PLAN_REFAKTORU_2026-10-11.md). Świeży CTest **7/9 PASS**, dwa zestawy zawierają ten sam runtime RED 32 konwersji Anycubic→CTB/AES. [Ocena każdego TODO](audit-2026-10-11/TODO_ASSESSMENT.md). W tej fazie nie wdrożono poprawek produkcyjnych ani nie zamknięto funkcji. W tym historycznym punkcie wdrożenie czekało na zatwierdzenie; zgoda i postęp są opisane powyżej.

**Bieżący audyt 2026-10-11:** [pełny status refaktoru](STATUS_REFAKTORU_2026-10-11.md). Fractional CTB lift PASS; Anycubic→CTB/AES RED 32/32. Starsze liczby poniżej odnoszą się do swoich checkpointów.

**Aktualizacja K02, 2026-10-09:** [fractional lift CTB/AES](FRACTIONAL_LIFT_2026-10-09.md), native 8526/38, CTest 9/9, 16 konwersji/32 Full Open ze źródłami. K01 Anycubic→CTB i reszta celu otwarte.

**Najnowsza partia A06.5, 2026-10-09:** [konwersja importowanego archiwum w GUI](IMPORT_CONVERSION_GUI_2026-10-09.md). Capture → wybór CTB/AES → nowy plik, preferencja drukarki i zachowany stan źródła. Wspólny schemat pól i odmowa cichej zmiany grayscale. Native 8382/37, CTest 9/9, 64 konwersje z independent readback oraz jeden GUI workflow. Pełny goal/E1/E2 i pozostałe rodziny nadal otwarte.

**Najnowsza partia A06.5 — 2026-10-09:** [cała tabela parametrów jako jedna transakcja](LAYER_CONVERSION_TABLE_2026-10-09.md). Zlikwidowano kolizje pośredniego Z i zależność od kolejności aktualizacji wierszy; 64 kombinacje CTB/AES, native 8376/36, independent 128 Full Open/384 warstwy. Pełny rejestr capabilities, fresh table, GUI konwersji importu i E1/E2 nadal otwarte. Starsze wpisy opisują swoje checkpointy.

**A06.5, późniejsza partia 2026-10-09:** [natywne tabele parametrów podczas konwersji CTB](LAYER_CONVERSION_2026-10-09.md). Poprawiono cichą utratę parametrów pristine importu i przekazanie edycji; brak pola docelowego blokuje publikację. API ma 8 odebranych konwersji; GUI importu/pełna macierz nadal otwarte. [Aktualny Kanban](STATUS_KANBAN_REFAKTORU_2026-10-09.md).

**Aktualny przegląd 2026-10-09:** [status funkcjonalny, architektura, pionowy Kanban, zależności i TDD](STATUS_KANBAN_REFAKTORU_2026-10-09.md). Ostatnia partia: 157 opcji + 3 ID + 15 relacji presetów; aktywny model drukarki w eksporcie. Native 1188/31, CTest 9/9, pięć GUI workflow i niezależny odczyt 11 plików/39 warstw PASS. Pełny goal oraz E1/E2 pozostają otwarte. Starsze wpisy poniżej opisują swoje checkpointy.

**Odebrany fragment 2026-10-09 — V03/A04:** preferencja formatu przez tożsamość źródłową drukarki po imporcie 3MF; native RED→GREEN 1005/22, GUI RED→GREEN (edytor i sidebar), CTest9/9 (37.11 s), UVtools Full Open CXDLPv4/6 warstw z niezależnym porównaniem kodowania 7-bit, Z, ekspozycji i ekranu. Pełne profile i refaktor nadal otwarte. [Raport](PRINTER_EXPORT_IDENTITY_2026-10-09.md).


**Późniejsza korekta A04.2-P:** persisted SLA role identities zachowane niezależnie od contextual labels GUI; native991/21, CTest9/9, GUI3MF/restart i independent exact settings157+3names. Poprzedni prefix identity FAIL naprawiony; pełne preset metadata/CRUD nadal otwarte. [Raport](PROJECT_PRESET_IDENTITY_2026-10-08.md).

**Najnowszy A04.3-M:** stale print/source guards i atomowy refresh pending project settings; native656/7, CTest9/9, model3MF GUI/restart/SL1, 1 export/6 original warstw. Efektywne wartości zachowane; nazwy presetów mają prefix po imporcie3MF — pełne identities/CRUD nadal otwarte. [Raport](MODEL_PROJECT_INTEGRITY_2026-10-08.md).

**Najnowszy V21/A04.3-G:** tworzenie stationary PCB job przeniesione z GUI do domeny; native432/17, CTest9/9, trzy GUI Save/Open/restart (PCB obie orientacje i calibration portrait), 9 UVJ/72 original/independent warstwy. Pełne parsery/receptury PCB, presety/model i konwersja nadal otwarte. [Raport](PCB_PROJECT_DOMAIN_2026-10-08.md).

**Późniejszy A04.3-G:** generated project Save/Open/restart z własnym config/Z, GUI/dirty/export; native1060/11, CTest9/9, GUI generator + imported regression, 3 UVJ/60 original warstw. PCB/orientacje/receptury generatorów, pełne presety i modelowy workflow nadal otwarte. [Raport](GENERATED_PROJECT_LIFECYCLE_2026-10-08.md).

**Najnowsze A04.2 effective settings:** 157 opcji SLA (32 printer/66 material/59 process), trzy nazwy, preferred format/version; lokalny restore, GUI capture/review/replacement i project dirty. Native7537/18, CTest9/9 (36.77 s), dwa project GUI + native regression, 11 original/independent plików/33 warstwy. Pełne presety/relacje/edycja i generated/model workflow nadal otwarte. [Raport](PROJECT_SETTINGS_SNAPSHOT_2026-10-08.md).

**Najnowsza integracja A04.3 GUI:** imported Save/Open/restart `.sla-project`, zapisany kontekst i osobne dirty baseline. Dwa Anycubic workflows + jedna native regresja passed; original/independent7 plików/21 warstw; CTest3/3. Pełne presety i pozostały A04 nadal otwarte. [Raport](PROJECT_LAYER_GUI_2026-10-08.md). Starsze wpisy opisują zakres swoich partii.

**Najnowsza partia A04.3 (backend):** origin model/import/generator, atomowy zapis projektu warstw i read/write cancel; native7340/16, 18 Full Open/8048 warstw, independent raw/pixels, CTest9/9 (36.68 s). Brak nowego GUI acceptance. Pełne presety A04.2 i Save/Open/restart A04.3 otwarte. [Raport](PROJECT_LAYER_ORIGIN_2026-10-08.md). Bieżący status ma pierwszeństwo przed historycznymi wpisami poniżej.

**A04.2 — fragment trwałego kontekstu eksportu:** API v2 zachowuje LCD/gęstość/cenę/max Z; brak/null pozostaje opt-out. Naprawiona granica float dla source 4000 warstw na max Z. Native7285/14, original18 plików/8048 warstw, dwa finalne GUI+4 Full Open/12 warstw, CTest9/9 (36.16 s), zero INET w monitorowanych GUI. A04.2 pełne presety/ustawienia i A04.3 GUI projektu nadal otwarte. [Raport](PROJECT_EXPORT_PROFILE_2026-10-08.md).


**A04.1 — późniejsza walidacja native snapshotu:** zgodność wariantu/geometrii/relacji SUBIMGS, znany limit Z, readonly i kanoniczne pola/float. Native6827/10, 68 byte-identical par, 68 Full Open/204 warstwy, CTest9/9 (35.75s). PWC1/515 pozostaje bez invented limit Z; A04.2 trwały kontekst i A04.3 GUI/restart nadal otwarte. [Raport](PROJECT_DOCUMENT_VALIDATION_2026-10-08.md). Starsze wpisy poniżej mają zakres swojej partii.


**Aktualizacja A04.1 — snapshot projektu v2 częściowo odebrany:** params/main/secondary/shared/native bytes i odczyt v1; native5683/10, 68 byte-identical par, 68 Full Open/204 warstwy, CTest9/9 (35.09s). A04.2 profile, A04.3 GUI/restart, hardening i konwersja nadal otwarte. [Raport](PROJECT_DOCUMENT_V2_2026-10-08.md). Starsze informacje o braku nowego schematu/RED dotyczą stanu przed tą partią. Główny goal i E1/E2 nadal aktywne.


**Bieżący status po audycie 2026-10-08:** [skrót i pionowy kanban](STATUS_GLOWNEGO_REFAKTORU_2026-10-08.md), [pełny backlog V01–V27](KANBAN_REFAKTORU_2026-10-08.md). Ostatnia odebrana partia to A04.2 opcjonalny profil eksportu przez API projektu i granica max Z. A04.1 snapshot/validation ma GREEN; pełne presety A04.2 i GUI/restart A04.3 nadal otwarte. Bieżące streszczenie ma pierwszeństwo przed opisami historycznymi. E1/E2 i główny goal nadal otwarte. **Wpisy poniżej są historią partii; ograniczenia już naprawione nie opisują stanu bieżącego.** Ta aktualizacja dokumentacji nie uruchamiała nowych testów aplikacji.


**Anycubic ruch warstwy — A06 częściowe, 2026-10-08:** Z/exposure/stored lift distance/lift speed edytowane w GUI/Undo/native save; globalne EXTRA/waits/retract readonly, Basic respektowany. Native38866/48, 22 warianty/68 Full Open/204 maski, cztery GUI/8 plików/24 maski, CTest9/9 (35.56s). Pełna macierz/capabilities, konwersja/projekt i E1/E2 nadal otwarte. [Dowody](ANYCUBIC_LAYER_MOTION_2026-10-08.md).


**CTBEncrypted parametry — A06 częściowe, 2026-10-08:** v4/v5 mają GUI/Undo/native save dla Z/exposure/light-off, ruchów/faz/waits/PWM. Native36446/43, 16 wariantów/48 Full Open/144 maski, cztery GUI/8 plików/24 maski, CTest9/9 (34.92s). Tilt readonly motion i zero-time mają regresje. Anycubic pełne ruchy, pozostałe capabilities, konwersja/projekt i E1/E2 nadal otwarte. [Dowody](CTB_ENCRYPTED_LAYER_PARAMETERS_2026-10-08.md).


**CTB parametry warstw — A06 częściowe, 2026-10-08:** ordinary v3/v4/v5/GKtwo: Z/exposure/light-off/ruchy/PWM, w v4+ dwie fazy i waits, GUI/Undo/native save. Native35390/41, 16 wariantów/48 Full Open/144 maski, independent32 edytowane/96 masek, cztery GUI/8 plików/24 maski. AES parametry, Anycubic pełny ruch, konwersja/projekt i E1/E2 nadal otwarte. [Dowody](CTB_LAYER_PARAMETERS_2026-10-08.md).


**Parametry warstw 2026-10-08 — częściowe A06:** typowany model/capabilities, Z i ekspozycja Anycubic edytowane w GUI/Undo/native save; ruchy na razie readonly. Native30499/39, 42 pliki/126 masek original, cztery GUI/8 plików/24 maski, CTest9/9 passed. Pełne motion/waits/PWM, inne rodziny, konwersja i projekt nadal otwarte. [Dowody](ANYCUBIC_LAYER_PARAMETERS_2026-10-08.md).


**SUBIMGS plane editor 2026-10-08:** jawny wybór main/secondary, pędzel/operacje/range/Undo/Redo i native zapis 518. Native 29745/38; 16 wariantów PWC/PM5S/M5SP/PM4N, 64 pliki/192 main/192 secondary independent (original tylko main), pięć GUI/10 plików i CTest9/9 passed. A03 potwierdzony w opisanym zakresie; A06 per-layer/capabilities, A04 projekt, real files i E1/E2 otwarte. [Dowody](ANYCUBIC_PLANE_EDITOR_2026-10-08.md).


**SUBIMGS 2026-10-08 — odebrany fragment A03:** main edit zachowuje independent masks, shared podąża za main. Native 2071/7, source/original 24 pliki (72 main/72 secondary niezależnie), trzy GUI i 6 plików/18 masek każdej grupy, CTest 9/9 passed. Wybór/edycja secondary nadal otwarte; A03/V07/V10/E2 niezamknięte. [Dowody i granice](ANYCUBIC_SUBIMAGES_2026-10-08.md).


**Przegląd bieżącego stanu 2026-10-08:** [skrót statusu, kanban i roadmapa](STATUS_GLOWNEGO_REFAKTORU_2026-10-08.md). A03/SUBIMGS ma wstępny backend GREEN 2035/7 i source-check 24 plików, bez finalnego GUI/CTest/checkpointu; wybór/edycja drugiej maski nadal otwarte. Pełny kanban zawiera aktualne blokady i backlog TDD. Starsze wpisy poniżej są przypisane do swoich partii.


**CTBEncrypted editor 2026-10-08:** native import/edycja v4/v5, AES ustawień i częściowego RLE, XOR, checksum/podpis/trailer. Native 6801 asercji/9 przypadków, 48 plików/144 maski original/source (w tym małe AES), cztery GUI/8 plików/24 maski, CTest 9/9 passed. Wspólny Gray7 pod regresjami; cancel/Undo/no-op zachowane. Pełne per-layer/SUBIMGS/profil 3MF/pozostałe importery i E1/E2 nadal otwarte. [Dowody](CTB_ENCRYPTED_EDITOR_2026-10-08.md).

**CTB modern editor 2026-10-08:** natywny import/edycja/zapis CTB3/4/5 i GKtwo; zachowane oba rekordy warstwy, maski, parametry i metadane. Native 1818 asercji/6 przypadków, 32 pliki/96 masek original/source, cztery GUI (8 plików/24 maski), CTest 9/9 passed. Zapis bez zmian/po Undo byte-identical; niejednoznaczne definicje odrzucone. CTBEncrypted/import Klipper/SUBIMGS/per-layer oraz E1/E2 nadal otwarte. [Dowody](CTB_MODERN_EDITOR_2026-10-08.md).

**Profil eksportu warstw 2026-10-08:** jawny LCD/materiał dla statystyk Anycubic, 1855 asercji/6 przypadków, 4 native/original pliki oraz 4 GUI/12 masek passed (w tym portrait); CTest 9/9. Negatywny GUI poprawiony przez odroczenie dialogu GTK; parametry/stałe przypisanie projektu/SUBIMGS i E1/E2 nadal otwarte. [Dowody i ograniczenia](LAYER_EXPORT_PROFILE_2026-10-08.md).

**Anycubic editor 2026-10-08:** native import/edycja 51 kombinacji × dwie orientacje, 1792 asercje/4 przypadki, 204 original/source pliki/612 masek, PWS AA1–16 32 pliki (maski/raw passed; global volume original FAIL z defektem cache), 4 GUI i CTest 9/9 passed. Independent SUBIMGS, binding geometrii/materiału, pełne per-layer i E1/E2 nadal otwarte. [Zakres i dowody](ANYCUBIC_EDITOR_2026-10-08.md).

**Aktualizacja 2026-10-08:** native Klipper MSLA Mono/RGB, typed destination i zatwierdzanie pól przy zapisie presetów: GUI/Full Open/source-check passed, 2326 asercji / 11 przypadków, CTest 9/9. Fresh audit 80 zapisów / 78 Full Open / dwa wyjątki original. Ścisły Roundtrip 48 zmiennych-Z/zero-wait próbek FAIL z opisanymi defektami referencji; 4 GUI RT bez różnic. Import/per-layer/pełne opcje oraz E1/E2 nadal otwarte. [Dowody i ograniczenia](KLIPPER_MSLA_2026-10-08.md).

**Bieżący status funkcjonalności i architektury: [Kanban 2026-10-08](KANBAN_REFAKTORU_2026-10-08.md).** Anycubic import/edycja oraz jawny profil eksportu odebrane w opisanym zakresie (najnowsze 1855 asercji / 6 przypadków, cztery GUI i CTest 9/9). Import/edycja ordinary CTB/GKtwo oraz CTBEncrypted4/5 odebrane w określonym zakresie; następne SUBIMGS i kontrakt per-layer; SUBIMGS, trwałe profile i pełne per-layer otwarte. Główny goal aktywny, bramka E1/E2 otwarta; szerokie UI/PCB/kalibracje i SLA-only nadal przed nami. Wyniki są przypisane do checkpointów.
**Wdrożone — SL1S eksport i atomowy SL1/SL1S:** osobny wybór `SL1S`
w presecie/panelu/edytorze. Błąd wysokości nie nadpisuje poprzedniego ZIP.
827 asercji / 9 przypadków, 32 pliki / 96 masek source verified, 32 original
Roundtrip bez różnic. GUI CRUD/restart, panel/lista/sufiks i edytor passed;
dwa końcowe GUI original Roundtrip po 6 warstw, zero różnic. Trzy końcowe
GUI monitorują app, zero INET. CTest 9/9, 33.19 s. Fresh audit 78 eksportów /
76 Full Open / dwa znane wyjątki. Zbiorcza macierz: zero luk wersji;
pozostaje brak archive evidence dla Klipper ZIP/RGB.ZIP. Ich legacy aliasy
przy SL1 nie zapisują kontenera Klipper — osobny native writer w kolejce.
E1/E2 oraz E3–E6 nadal otwarte.
[Dowody i ograniczenia](SL1S_EXPORT_2026-10-07.md).

**Wdrożone — Anycubic legacy 1/515:** poprawione tabele/ColorTable,
walidacja, strumieniowy i atomowy zapis. DL2P 515 ma prawidłową geometrię
D2; PWC 1/515 jawnie informuje w GUI o braku MACHINE i odczycie wymiarów
jako zero. 48 native plików / 144 maski source verified, 48 original
Roundtrip par bez różnic w 42 polach i warstwach. Trzy GUI eksporty
PWC 1/515 i DL2P 515 oraz original Roundtrip passed, monitor zero INET.
Końcowa kompilacja passed, native 9307 asercji / 5 przypadków,
CTest 9/9 (32.38 s). 48 finalnych fixture bajtowo identycznych z próbkami UVtools.
Wraz z poprzednimi 27 modern kombinacjami pokryto wszystkie 51 par
Anycubic; zbiorczy audyt ma zero luk wersji i zero niedozwolonych wersji.
To nie zamyka E1/E2: import/per-layer, aliasy SL1S i Klipper ZIP/RGB.ZIP,
pełne parametry i pliki urządzeń nadal w kolejce; E3–E6 otwarte.
Wybór wersji zapisywany w presecie, filtrowanie list i zachowanie Undo
sprawdzono w poprzednim etapie. Poprawiony zapis tymczasowy 1–518 obsługuje
długie nazwy docelowe bez wydłużania komponentu ścieżki.
[Dowody i ograniczenia](ANYCUBIC_LEGACY_2026-10-07.md),
[wybór wersji](ANYCUBIC_VERSION_SELECTION_2026-10-07.md).

**Poprzedni checkpoint E1/E2 (2026-10-07):** PM5/PM3N/PX6S używają 517;
PM5S/M5SP mają natywną strukturę 518 i dwa poprawne RGB565 previews.
2898 asercji, 20 plików / 60 masek source verified, original Roundtrip
oraz dwa GUI eksporty passed. Wówczas pozostawało 14 wierszy z lukami wersji; zero
zaobserwowanych niedozwolonych wersji. 516/517/518 mają teraz wspólny poprawiony writer, TSMC i różny
bottom/normal retract: 28 próbek source verified, 14 original-setter par
(42 pola), GUI 516/517 i CTest 9/9 passed. Kontrakt 22 rozszerzeń zgodny
z original API. Wybór wersji w presecie zamknięto w nowszym checkpoint. Legacy 1/515 zamknięto w nowszym checkpoint;
per-layer/import Anycubic, aliasy i pełne parametry nadal wymagają pracy. 75 Full Open nie zamyka zgodności.
[Wspólny writer 516/517/518](ANYCUBIC_MODERN_2026-10-07.md),
[TSMC 518](ANYCUBIC_TSMC_2026-10-07.md),
[518 i kolejka](ANYCUBIC_518_2026-10-07.md),
[macierz referencyjna](VERSION_MATRIX_2026-10-07.md).

Aktualny audyt eksportu (2026-10-06): 77 ID, 77 zapisanych, 75 pełne
odczyty UVtools i dwa znane wyjątki dekodera (SL1SVG/CXDLP v1).
NanoDLP ma profil kontrolera w presecie oraz cztery warianty Mono/RGB v1/v2.
Import/zapis/restart profilu i cztery eksporty GUI przeszły. UVtools Roundtrip:
42 pola i 6 warstw na wariant, zero różnic.
CBDDLP/Photon v1/v2: dwa preview, AA1–16 w presecie, zmiana AA bez utraty
edycji warstw. 68 plików / 272 maski Full Open i niezależnie zweryfikowane.
Cztery eksporty GUI oraz CRUD presetów z AA16 passed; CTest 9/9.
Roundtrip AA1/2/4/8/16 zgodny, inne AA mają udokumentowaną utratę bieli
w oryginalnym UVtools; surowe niezaliczone raporty zachowane.
Szczegóły: [CBDDLP/Photon versions](CBDDLP_PHOTON_VERSIONS_2026-10-06.md).
CTB v1/v2: dwa podglądy, poprawione adresy tabel i payloadów; pełny odczyt,
niezależne maski/parametry i Roundtrip passed dla czterech próbek oraz dwóch
scenariuszy GUI. [CTB legacy previews](CTB_LEGACY_PREVIEWS_2026-10-06.md).
Edytor otwiera i zapisuje natywnie CTB/CBDDLP/Photon v1/v2, także seeded CTB.
136 oryginalnych plików / 544 maski importu i byte copies passed;
72 pary edycji / 280 masek zweryfikowane niezależnie z UVtools Full Open.
Pięć GUI workflow i CTest 9/9 passed. Importery nowszych wersji, kompaktowanie
oraz przerwanie każdego kodeka pozostają otwarte.
[Chitubox editor](CHITUBOX_EDITOR_2026-10-06.md).
Import i zapis oryginalnego archiwum mają Cancel, checkpoints i atomowy
rollback. 440 asercji / 18 wariantów, cztery GUI workflows i CTest 9/9 passed.
Zapis 300-warstwowego CTB zatrzymany w około 0.301 s w próbie GUI.
Pozostaje Cancel eksportu ze sceny 3D, wszystkich kodeków i globalnego
slicingu/hollowing. [Archive cancellation](LAYER_ARCHIVE_CANCEL_2026-10-07.md).
Zapis pliku nie certyfikuje urządzenia.
Szczegóły: [CTB modern](CTB_MODERN_2026-10-06.md) oraz
[NanoDLP GUI](NANODLP_GUI_2026-10-06.md). Starsze tabele są historycznymi zapisami,
a nie dzisiejszym kompletem obsługi.

Lista odpowiada etapom A–L z PLAN_DALSZYCH_PRAC.md. Żaden etap nie jest
zamknięty wyłącznie na podstawie obecności klasy, menu lub rozszerzenia.
„Częściowo” oznacza działający zakres z nadal niespełnionym kryterium odbioru.

| Zakres | Status | Wdrożone / dowód | Pozostaje |
|---|---|---|---|
| A. Macierz wymagań | Częściowo | Inwentaryzacja 58 ID, 31 rodzin, 53 plików operacji | Wersje, aliasy, test i źródło dla każdej funkcji |
| B. Geometria | Częściowo | Wspólny raster, stół z ekranu, SL1 portret, testy odbić i prostokątnych pikseli | Pełny odczyt UVtools wszystkich kodeków, SVG i stare nieoznaczone pliki |
| C. Profile | Częściowo | Domyślne SLA, synchronizacja stołu, format w presecie, walidacja indeksów list | Pełny test GUI tworzenia/kopiowania/usuwania/importu i restartu, wersje formatów, parametry ruchu |
| D. Dokument warstw | Częściowo | SL1/PNG, historia, atomowy zapis, limity pamięci | Wieloformatowe metadane, wspólny zapis projektu 3D i edycji, magazyn dyskowy, odzyskiwanie |
| E. Formaty | Częściowo | 28 ID generuje pliki, 30 blokowanych; regresje SL1 i nagłówka CTB | Natywne czytniki binarne, komplet kodeków i niezależna walidacja; zapis nie oznacza zgodności |
| F. Główne UI | Częściowo | Renderer, slicing, edytor dostępny z menu i po slicingu | Pełny scenariusz GUI, wszystkie listy, anulowanie, błędy i skróty |
| G. Operacje warstw | Częściowo | Okrągły ślad, próg, odwracanie maski, Undo/Redo | Ciągły pędzel, zaznaczenia, figury, zakresy, analiza wysp, reszta operacji UVtools |
| H. PCB | Brak implementacji | Wymagania opisane w planie | Import, maski, fizyczna skala, ekspozycja, GUI i eksport |
| I. Kalibracje | Brak kompletnego modułu | Wymagania opisane w planie | Generatory, GUI, zapis i stosowanie wyników do profili |
| J. Skrypty i funkcje pomocnicze | Brak zgodności | Inwentaryzacja źródeł | Jawny kontrakt obsługi i implementacja funkcji referencji |
| K. Refactor i FFF | Częściowo | Wspólna geometria, ograniczenia UI/offline; usunięta nieużywana fabryka zastępczych writerów | Usunięcie zależności FFF i pełny audyt połączeń |
| L. Wydanie i odbiór | Niezakończony | Instrukcja, lokalne buildy i testy SLA | Czysta instalacja/pakiet, komplet scenariuszy i macierz urządzeń |

## Konkretne poprawki z ostatniego zgłoszenia @todo wciąż do zrobienia 

- [ ] Synchronizacja stołu z prawidłowymi wymiarami ekranu w pełnej konfiguracji SLA.
- [ ] Automatyczna normalizacja rozszerzenia przy wyborze formatu eksportu.
- [ ] Format jest częścią presetu drukarki; wymaga zwykłego zapisu presetu.
- [ ] Wszystkie listy rozwijane sprawdzone i naprawione (obecnie zabezpieczono indeksy presetów). Powinno się dać zieniać wybory za pomocą myszki a aktualnie listy same się zamykają w takim wypadku przed dokanaiem woboru, albo wybory nie są tymi któ©e wybrałem
- [ ] Odbiór GUI edytora po slicingu (przycisk i połączenie są zaimplementowane).
- [ ] Wszystkie formaty otwierane w UVtools i porównane piksel po pikselu.
- [ ] Całkowite usunięcie FFF/G-code z UI i zależności oraz kodu .
- [ ] Pełna weryfikacja braku połączeń sieciowych.
- [ ] Wszystkie funkcje UVtools dostępne z GUI, w tym PCB i kalibracje.
- [ ] Benchmark dużych modeli i pełna optymalizacja slicingu (gamma nie powtarza już cięcia).
- [ ] nie wszystkie formaty zapisu plikóœ dla drukarek sla działają po exporcie, aby weryfikować uzyj UVtools jako import pliku i jeśli tam się otwiera poprawnie to na 80% jest ok, ale aby mieć 100% pewności, exportuj model jako sl1, konwertuj go w UVtools do formatu docelowego i porównaj z formatem docelowym exportowanym bezpośrednio z naszej apki, jeśli coś się nie zgadza, choćby jeden bit, znaczy ze trzeba poprawić sposób zapisu do naszego formatu plikóœ bo UVTools robi to poprawnie.


## Weryfikacja

Wyniki po usunięciu nieużywanej fabryki i dodaniu odwracania maski:
5/5 zestawów CTest SLA; 1075 asercji w 17 wybranych regresjach.
Aplikacja i testy zostały przebudowane; launcher przechodzi `--help`.
Logi: `build/todo-cleanup-build-j4.log`, `build/todo-tests-final-build.log`,
`build/todo-ctest.log`, `build/todo-regressions.log`.
Przycisk Invert layer skompilowano, lecz nie wykonano jego testu interakcji GUI. Szczegóły i ograniczenia dekodera/GUI:
[WORKFLOW_AUDIT_2026-09-23.md](WORKFLOW_AUDIT_2026-09-23.md).
Ponowiona próba skryptem `tests/sla_print/audit_uvtools.py`: 27 blokad natywnej
zależności, 1 błąd dekodera (SL1SVG), 30 pozycji bez eksportu. Wyniki:
`build/sla-format-audit/uvtools-current/results.json`.
Nie wykonano fizycznej walidacji drukarek. Nie należy oznaczać powyższych
otwartych pozycji jako zakończonych po samym udanym buildzie.

## Partia profili — 2026-09-24

Dodano listę identyfikatorów formatów w ustawieniach drukarki, dostępną w trybie
Simple. Poprawiono obsługę pojedynczych wartości tekstowych w edytowalnych
listach (inicjalizacja, Enter i utrata fokusu). Regresja potwierdza zapis/odczyt
dwóch niezależnych drukarek, aktualizację istniejącego formatu oraz usuwanie
kopii bez usuwania oryginału. To test logiki presetów, nie pełny odbiór GUI.
Aktualne wyniki: 5/5 CTest SLA, 1098 asercji w 18 wybranych przypadkach.
Cały etap C i wszystkie listy GUI pozostają otwarte.

## Aktualizacja 2026-10-02

Bieżąca partia: [WDROZENIE_2026-10-02.md](WDROZENIE_2026-10-02.md).
Potwierdzono kompilację, 4/4 skupione zestawy CTest, GUI kreatora i podstawowy
przebieg edytora po slicingu. 29 formatów zapisuje małe pliki testowe, 29 jest
zablokowanych. Odczyt UVtools pozostaje zablokowany zależnościami dla 28
plików; SL1SVG nie został rozpoznany. Nie oznaczać wszystkich formatów,
całego GUI, PCB, kalibracji, operacji UVtools ani usunięcia zależności FFF
jako zakończonych. Starsze wpisy w tym pliku nie zastępują aktualnej walidacji.

## Aktualizacja zweryfikowana 2026-10-03

Historyczne liczby i checkboxy wyżej nie opisują bieżącego wydania.
Rejestr nadal ma 58 ID; 34 generuje pliki, 24 ma jawną blokadę. Nie oznacza to
pełnej zgodności 34 formatów. Format eksportu jest niezależną preferencją
per drukarka w AppConfig; kopia, rename, delete i restart są sprawdzone dla
obsługiwanego przepływu GUI. CRUD drukarki/materiału/procesu przeszedł test GUI.
Edytor po slicingu jest sprawdzony w uruchomionej aplikacji; ma ciągły pędzel,
zoom, zakresy, grouped Undo/Redo i 7 operacji morfologicznych.
Pełny port UVtools, wszystkie czytniki/kodeki, PCB, kalibracje i wydanie nadal
nie są ukończone. Aktualne dowody: [raport](WDROZENIE_2026-10-03.md).

## Kontynuacja 2026-10-03 — zadania rastrowe i GUI

Stan wcześniejszych tabel jest historyczny. Bieżąca kontynuacja dodaje natywne
zadania bez modelu 3D, PCB Gerber/Excellon z makrami apertur oraz pierwsze
kalibracje XYZ/grayscale/lift. Szczegóły i jawne ograniczenia są w
PCB_CALIBRATION.md i WDROZENIE_2026-10-03.md. Nie oznacza to zakończenia etapu
PCB ani kalibracji: pozostałe polecenia i wzorce referencji nadal wymagają portu.

Potwierdzono 5/5 skupionych zestawów CTest, 105 asercji PCB/kalibracji przed
ostatnimi dodatkami, GUI generowania obu typów zadań i prawidłowe anulowanie
morfologii w działającym edytorze. Zmiany masek/Undo/Redo pozostają nienaruszone
po Cancel. GUI zwykłego modelu przeszło slicing, operacje zakresowe, Apply,
rzeczywiste zamknięcie i ponowne otwarcie.

Domyślna zwykła i początkowa warstwa SLA wynosi 0,05 mm; nowy szablon nazwy
używa wybranego rozszerzenia. Zapisane profile użytkownika nie są migrowane
przez samą zmianę wartości domyślnej.

Nadal pozostają 24 jawnie blokowane ID eksportu, natywne czytniki innych
formatów, pełne operacje i kalibracje UVtools, wspólny zapis sceny i masek,
pełne usunięcie zależności FFF, odbiór offline i gotowy pakiet instalacyjny.
Nie deklarujemy zakończenia całości ani zgodności z każdym firmware.

## Dopisek 2026-10-03 — N4/N7, OSLA i statystyki edycji

Aktualny audyt ma 37 written, 21 blocked i 0 error. Odblokowano natywne
N4/N7 i OSLA po porównaniu masek, układu bajtów oraz oryginalnych struktur
UVtools. OSLA przeszło też parser komend UVtools, z porównaniem wszystkich
parametrów ruchu/ekspozycji z tabelami. Nazwy UTF-16BE Anet i UTF-8 OSLA
sprawdzono na polskich znakach. Szczegółowe ograniczenia: manual.txt oraz
doc/WDROZENIE_2026-10-03.md. Nadal brak pełnego natywnego dekodera UVtools
w tym środowisku i odbioru na fizycznych drukarkach.

Apply oraz eksport edytowanych masek modelu 3D mają aktualne statystyki
materiału i czasu. Eksport bez Apply nie zmienia aktywnego wyniku. Ponowne
process() po zmianie samej ekspozycji/ruchu zachowuje te statystyki.
Skupiona regresja po zmianach: native-codecs-ctest.log, 5/5 zestawów.
To kontynuacja refaktora; wcześniejsze wpisy statusu nie są certyfikatem
zakończenia funkcji, a pozostałe zadania nadal wymagają wdrożenia i sprawdzenia.

## Projekt i wynik edycji — 2026-10-03

Podstawowy wspólny zapis sceny i zastosowanych masek 3MF działa i przeszedł test
GUI po restarcie (gui-project-final/result.json). Zapis jest atomowy, odcisk źródła
zapobiega zastosowaniu starych masek do zmienionej geometrii, a odczyt podpór/
otworów zachowuje ich współrzędne. Eksport edycji ma aktualne statystyki i podgląd
pierwszej maski. Pozostają magazyn dyskowy, odzyskiwanie oraz pełny zakres UVtools.

## Zweryfikowana partia 2026-10-04: Creality

- CXDLP v1/v2/v3: eksport, samodzielny import/edycja/zapis, sumy i metadata;
  native + oryginalne klasy UVtools + GUI. Wybór v2 jest osobnym presetem.
- CXDLPv4: poprawne 32-bajtowe podglądy RGB15, kwantyzacja/volume/contours;
  native + oryginalne klasy UVtools + GUI eksportu oraz samodzielnego
  edytora (także szyfrowanie). Referencyjny plik 460 warstw: GUI pass.
  Pełny UVtools Open i firmware nadal brak.
- 12 szablonów Halot/CT: model, format, niezależność i fizyczny bed_shape;
  pole modelu dostępne i edytowalne w panelu. GUI wizard/restart/CRUD pass.
- Aktualny audyt: 66 ID / 47 written / 19 blocked; braki 13 rozszerzeń.
  Liczniki nie stanowią certyfikacji całego zakresu UVtools.
- Dowody i szczegółowe ograniczenia: WDROZENIE_2026-10-04.md.

## Zweryfikowana partia 2026-10-04: CLI SLA

Domyślne SLA, obrys zgodny z LCD, suffix wybranego formatu, brak kompilowanej
ścieżki eksportu FFF w CLI oraz brak post-process i uruchamiania downloadera.
Zapytania profili pokazują modele SLA i żywice; kody zakończenia poprawione.
Testy: sla_cli_smoke oraz regresja GUI krojenia/edytora/eksportu v4.
Pełne usunięcie zależności FFF w dziedziczonym rdzeniu i GUI nadal otwarte.

## Zweryfikowana partia 2026-10-04: wiele plików PCB

Scalanie albo osobne warstwy, wspólne centrowanie/odbicia, wspólne wiercenia,
wybór wielu plików w GUI oraz eksport. Native i GUI z pełnym porównaniem
pikseli, oryginalny schemat UVJ UVtools, 7/7 CTest. Szczegóły w raporcie
wdrożenia. Oddzielne przesuwanie i polaryzacja źródeł w GUI nadal otwarte.

## Zweryfikowana partia 2026-10-04: Box blur

Operacja zakresu w GUI z promieniem, szarością, atomowym Undo/Redo i
anulowaniem. 1200 obrazów identycznych z cv::blur; 27 asercji dokumentu;
7/7 CTest; GUI krojenie/edycja/eksport v4 i kontrola braku sieci: pass.
OperationBlur pozostaje częściowy: 5 innych algorytmów nadal otwartych.

## Zweryfikowana partia 2026-10-04: XML.CWS

Osobny format cwsxml / .xml.cws: PNG32, manifest/profil XML, rzeczywiste
Z/ekspozycja/dwustopniowe ruchy i synchronizacja <Takes>. Import RGBA,
edycja i zapis z pełnym suffixem, zachowanie metadanych, ścisłe CRC PNG.
1872 asercje, 7/7 CTest (23 scenariusze CLI), GUI eksport/import/edycja
z pełnym porównaniem pikseli i brakiem sieci. Oryginalny schemat/parser
UVtools odczytał trzy pliki; parametry eksportu i zapisu edytora identyczne.
Audyt 64 ID / 45 written / 19 blocked, 14 rozszerzeń nieobecnych.
Nie oznacza to pełnego natywnego FileFormat.Open ani walidacji firmware.
NovaMaker slice.conf i pakowany RGB.CWS nadal wymagają implementacji.

## Zweryfikowana partia 2026-10-04: Median blur

Filtr medianowy w GUI z promieniem 0–4096, brzegami Replicate i zachowaniem
szarości. Histogramy 32-bitowe bez kosztu proporcjonalnego do powierzchni
filtra. 1200 obrazów zgodnych z lokalnym cv::medianBlur, 30 asercji zakresu,
Undo/Redo i anulowania; analityczne przypadki promienia 4096. 7/7 CTest
oraz GUI cięcie/filtr/historia/eksport XML.CWS/brak sieci: pass.
OperationBlur pozostaje częściowy: StackBlur, Pyramid, Gaussian i Filter2D
nadal otwarte. Szczegółowe wyniki i ograniczenia: WDROZENIE_2026-10-04.md.
Anulowanie w GUI dodatkowo potwierdzono na CXDLPv4 460 × 1620×2560
z promieniem 4096: bez zmian masek/historii i bez prób połączeń.

## Zweryfikowana partia 2026-10-04: NovaMaker slice.conf

Osobny wybór NovaMaker CWS (cwsnovamaker / .cws), 14 pól slice.conf,
grayscale PNG i pełne parametry ekspozycji/ruchu. Dotychczasowy cws/XML
pozostaje odrębnym schematem. Import/zapis po edycji, numeric layer order,
zachowanie ustawień/instrukcji/nieznanych wpisów i ścisła walidacja wejścia.
2518 asercji, 7/7 CTest (24 scenariusze CLI), GUI eksport/import/Undo/Redo/
zapis/piksele/brak sieci: pass. Oryginalne klasy UVtools odczytały 3 pliki
NovaMaker i 2 kontrolne CWS/XML; parametry przed/po edycji identyczne.
Aktualny audyt: 65 ID / 46 written / 19 blocked, 14 brakujących rozszerzeń.
RGB.CWS oraz pozostałe kodeki nadal wymagają wdrożenia. Natywne
FileFormat.Open i firmware nadal niezweryfikowane; pełny zakres nieukończony.

## Checkpoint do testów użytkownika 2026-10-04 — refaktor wstrzymany

RGB.CWS jest teraz zaimplementowany jako cwsrgb / .rgb.cws: pakowane BGR,
pełna geometria LCD, import/edycja/zapis i walidacja szerokości podzielnej
przez 3. 4974 asercje / 3 przypadki, niezależne OpenCV 102510 pikseli,
oryginalne parsery UVtools dla 3 plików oraz GUI eksport/edycja: pass.
Cache konwersji identycznych masek RGB/XML: 14 asercji, single-pair
benchmark eksportu płaskiego STL 4.31×; nie ogólnego slicingu.
7/7 CTest, 26 scenariuszy CLI. Poprawiono 11 szablonów CWS, native profile
tests 405 asercji / 15 przypadków. Automatyczny wybór Bene4 Mono w GUI
niezaliczony (test szukał tylko widocznego wiersza bez przewijania);
potrzebne ręczne potwierdzenie. Pozostałe rodziny profili nadal do audytu.
Aktualna macierz: 66 ID / 47 zapisanych / 19 zablokowanych,
13 nieobecnych rozszerzeń. Pełne FileFormat.Open, firmware i reszta
wymagań nadal niezweryfikowane lub nieukończone.

Szczegóły: WDROZENIE_2026-10-04.md. Osobny dokument użytkownika:
MID_REFACTOR_TESTY_2026-10-04.md — instrukcja startu, znane braki,
kolejka oraz nowe zgłoszenia. Do omówienia jego wyników nie kontynuujemy
głównego refaktoru. Nie oznaczono celu jako zakończonego.

Kontrola checkpointu: build pass, profile 405/15 pass, 7 testów SLA pass.
Pełny CTest niezaliczony: Archive export test / cxdlp odrzuca domyślny
wait before exposure; 124/125 przypadków zbiorczego sla_print_tests pass.
Dwa testy bez binariów Not Run. Problem K11 i pełne wyniki zapisane
w MID_REFACTOR_TESTY_2026-10-04.md; analiza przed wznowieniem prac.

## Plan po ręcznych testach użytkownika 2026-10-04

Użytkownik zgłosił blokadę eksportu wszystkich formatów, również SL1,
komunikatem unknown raster archive format, samoczynne zamykanie listy
formatów oraz problemy kreatorów i edytora. Są to zgłoszenia użytkownika;
przyczyna regresji nie została jeszcze odtworzona w tej sesji.
Nowa obowiązująca kolejność: E0 przywrócenie referencyjnego SL1 → E1
naprawa istniejących writerów, CXDLPv4 i GUI eksportu → E2 wszystkie formaty
i pełny odbiór UVtools → dopiero E3–E6 pozostały refaktor.
PLAN_PO_TESTACH_2026-10-04.md zawiera zgłoszenia R01–R14, roadmapę i bramki.
GOAL_EKSPORT_2026-10-04.md zawiera nowy cel oraz kryteria ukończenia.
W tej sesji przygotowano dokumentację, nie wdrażano poprawek aplikacji.
Poprzedni goal pozostaje wstrzymany; mechanizm sesji nie pozwolił utworzyć
nowego przy istniejącym niezakończonym celu. Nie uznano starego za ukończony.

## Odbiór SL1 / CXDLPv4 po wznowieniu 2026-10-04

Odtworzono w GUI zapis etykiety zamiast ID oraz dokładny komunikat
Unknown raster job archive format. Poprawiono mapowanie string choice,
kontekst docelowego eksportu i wejście w natywny dialog z panelu głównego.
Zapis ID, CRUD, eksport z edytora i panelu głównego, .3mf/restart: pass.
Oba formaty przeszły także GUI z ekranem CL-92 5760×3600 i stabilną listą
formatów. Native regresja: 54 asercje; build i 7/7 SLA CTest pass.

Pełny UVtools/OpenCV działa dzięki oryginalnej lokalnej bibliotece
build/platforms/linux-x64/libcvextern.so (Emgu/OpenCV 4.12), skopiowanej
wyłącznie do testowego build. Nie jest już blokowany przez FFmpeg.
FileFormat.Open → Encode → Open: wszystkie maski i raportowane parametry
SL1/CXDLPv4, także czas/objętość i pełny ekran CL-92, bez różnic.
Audyt pozostałych: 44 fixture z pełnym odczytem pikseli, CTB błąd RLE,
CXDLP v1 NullReference, SL1SVG nierozpoznany; 19 nadal blocked.
Odczyt 44 fixture nie dowodzi zgodności ich parametrów ani wszystkich wersji.
K11 i pełny szeroki CTest nadal otwarte; nie pomylono 7/7 SLA z pełnym odbiorem.

Na prośbę użytkownika SL1 i CXDLPv4 przekazujemy do jego weryfikacji przed
dalszą pracą. ODBIOR_SL1_CXDLPV4_2026-10-04.md zawiera instrukcję, dowody
i checklistę. Pozostały refaktor czeka na wyniki; cały cel nieukończony.

## Porównanie realnych eksportów i wydajność — 2026-10-04

Na prośbę użytkownika wykonano tylko naprawy eksportu/rasteryzacji i kontrolę
plików ../testexports; główny refaktor nadal wstrzymany przed jego kolejnym testem.

1180 warstw CL-92: oryginalne CXDLPv4 aplikacji i UVtools mają identyczne RLE,
piksele i parametry warstw. 158410 B różnicy to wyłącznie kompresja miniaturek.
Poprawiono ProjectorType (maski już zawierają transformację), RGB15 RLE preview,
równoległe statystyki i geometrię, ROI konturów, szybsze bezstratne PNG libpng/zlib.
SL1 zapisuje unikalne rozmiary miniaturek i czyta identyczne stare duplikaty.
Różne maski/ustawienia/miniaturki o tej samej nazwie nadal odrzucane.

Kontury aplikacji odpowiadają maskom po redukcji szarości 1; native Encode
UVtools potwierdził wszystkie 1180 powierzchni. Zapis objętości 52.582920 ml
potwierdza niezależna suma pikseli i fizycznej geometrii: 52.582919 ml.
UVtools dodaje 300 s do oszacowania czasu i zaokrągla objętości/ruch warstw.
Nie zmieniano poprawnych parametrów ruchu, aby dopasować te heurystyki.

Benchmark tego samego stosu: eksport SL1 16.69→4.82 s, CXDLPv4 71.74→11.40 s.
Natywne krojenie dwóch modeli 200-warstwowych: 2.25×/3.37×, wszystkie maski/Z
zachowane (porównanie przez ponowne kodowanie nowego obrazu starym encoderem).
Nie jest to pomiar pełnej rasteryzacji siatki modelu użytkownika.
Build + 7/7 SLA CTest + GUI obu eksportów z CL-92 pass. Oryginalny duży stos:
1180 PNG SL1 bajtowo zachowane; 1180 RLE i definicji CXDLPv4 bez zmian.
Oryginalny full UVtools: 42 pola globalne i wszystkie parametry/piksele warstw
nowego SL1 względem źródła oraz nowego CXDLPv4 względem konwersji użytkownika
bez różnic. Oracle odświeża teraz globalną objętość po zakończeniu równoległego
ładowania, żeby nie porównywać przypadkowej nieaktualnej sumy częściowej.

Raport: POROWNANIE_EKSPORTU_I_WYDAJNOSC_2026-10-04.md.
Dowody: build/export-performance-2026-10-04/. Uwagi użytkownika pozostawiono.
CTB/CXDLPv1, K11 i pozostałe formaty/UI/refaktor nadal w kolejce.

Dodatkowo pełny ekran edytora: operacje zakresowe, Undo/Redo, Apply, .3mf,
restart i eksport SL1 pass. Próba 400% przekracza istniejący limit bitmapy
32 mln pikseli i otwiera modalną informację; test poprawiono na dozwolony
zoom i prawidłowe wymiary CL-92. Renderowanie tylko widocznego fragmentu
przy dużym zoomie nadal do wdrożenia w etapie UI, nie było przedmiotem tej
optymalizacji. GUI SL1/CXDLPv4 po poprawkach również Full Open UVtools pass.
Checkpoint: build/export-performance-2026-10-04/checkpoint/.

## Wznowienie po testach — CTB v2 i K11

Na decyzję użytkownika główny goal jest ponownie aktywny. Naprawiono CTB v2
RLE i zapis czasu ruchu/odpoczynku. 228 asercji, duże próbki wszystkich odcieni,
pełny UVtools, niezależne PNG i ponowny Encode (42 pola globalne), GUI eksport
i Full Open: pass. 7/7 SLA pass. K11 w zbiorczym teście rozwiązany poprzez
jawne poprawne konfiguracje rodzin: 106 asercji, oba modele, pass. Audyt
produkcyjnych profili/materialów i ograniczeń w GUI nadal w kolejce.
CXDLPv1: oryginalny rejestr UVtools nie zawiera jego parsera; automatyczne
Full Open wybiera v2/v3 i nadal nie jest zaliczone. Pozostałe warianty CTB,
19 blokowanych ID, pozostałe formaty/UI i E3–E6 nie są zamknięte.
[Dowody, odtworzenie i następne kroki](WZNOWIENIE_REFAKTORU_2026-10-04.md).

Końcowa korekta CTB: objętość z maski po kwantyzacji, masa/koszt z tej
objętości oraz kontrola kompletności RLE i granicy 4 GiB przed zapisem.
Pełny CTest 10/10; po tej korekcie ponowiono wszystkie 9 zestawów poza
niezmienionym FFF sequential arrangement, 9/9 pass. Końcowy GUI i Full Open
ponownie pass. Raport i checkpoint w WZNOWIENIE_REFAKTORU_2026-10-04.md.

## CBDDLP/Photon v2 — kolejna partia E1–E2

Natywne writery AA=8 wdrożone i odblokowane; moduł Chitubox wspólny z CTB,
pełne maski, metadane, ekspozycje dolne/przejściowe/normalne, ruch jednoetapowy, deduplikacja
bitplanów. Flaga projekcji całej rodziny wynika z mirroringu profilu.
6577 asercji / 3 przypadki pass, GTK export i rzeczywiste zmiany końcówek
CBDDLP/Photon ↔ SL1 pass, pełny UVtools pass. Każde 24/24 RLE identyczne
z konwersją UVtools. Zwykły Convert pomija light-off warstw SL1; ten wynik
zachowano osobno. Oryginalne API kalkulatora/Encode UVtools z zachowanym
źródłowym odpoczynkiem daje identyczne 42 pola globalne i wszystkie warstwy.
Surowe różnice czasu, objętości przed/po kwantyzacji i masy są opisane jawnie.
9/9 CTest w tej partii pass. Audyt: 49 zapisanych ID, 47 pełnych odczytów,
SL1SVG i CXDLPv1 nadal nie przechodzą automatycznego odczytu, 17 blocked.
Nie ukończono wszystkich wersji/formatów/profili ani pozostałych UI/E3–E6.
[Dowody i ograniczenia](CHITUBOX_CBDDLP_PHOTON_2026-10-04.md).

## Korekta osobnego encrypted.ctb (2026-10-04)

Usunięto błędny alias zwykłego CTB v2. Osobny `ctb_encrypted` jest jawnie
niedostępny do czasu implementacji oryginalnego v5; bezpośredni writer też
nie może emitować CTB pod szyfrowanym sufiksem. Historyczny PHZ nie może
użyć kodeka CTB ze zmienionym magic. Nie jest to wdrożenie tych formatów.
6734 asercje / 10 przypadków, CTest 9/9, GTK CTB z przełączaniem sufiksów
oraz Full Open nowego pliku GUI pass. Świeży audyt: 67 ID, 49 written,
18 blocked, zero error. Pozostają własne kodeki encrypted CTB/PHZ, wszystkie
wersje/formaty i dalsze UI/E3–E6. Główny cel aktywny.
[Szczegóły i dowody](CTB_ENCRYPTED_REJESTR_2026-10-04.md).

## Photon S (.photons), 2026-10-05

Wdrożono natywny schemat AnycubicPhotonSFile: binarny RLE, nagłówki,
miniatura BGR565, dokładna objętość/pitch, wspólny ruch i light-off.
Walidacja ograniczeń Photon S działa przed slicowaniem; docelowa konfiguracja
edytowanego eksportu jest weryfikowana przed kodowaniem całego stosu.
86 996 asercji / 2 przypadki, CTest 9/9, GTK z przełączaniem sufiksów i
Full Open 6 warstw pass. Oryginalne Encode/Convert: wszystkie 42 pola
raportu i warstwy zgodne, każde 3/3 RLE identyczne bajtowo. Surowe różnice
objętości/XY pitch/Z float UVtools ujawniono osobno, nie ukryto tolerancją.
Nowy audyt full-oracle: 67 ID, 50 written, 48 pełnych odczytów, dwa znane
wyjątki SL1SVG/CXDLPv1, 17 blocked. Nie certyfikuje wszystkich wersji ani
firmware; pozostała kolejka E1–E6 i główny cel aktywne.
[Raport i dowody](PHOTON_S_2026-10-05.md).

## PHZ v2 (.phz), 2026-10-05

Wdrożono natywny nagłówek, grayscale RLE zgodny z PHZFile UVtools,
szyfrowanie zależne od indeksu warstwy, skompresowane miniatury, rzeczywiste
Z/ekspozycje/PWM/ruch/light-off oraz objętość zapisanej maski. Klucz uint32
(0 lub decimal/0x) jest opcją drukarki dostępną w GUI. Poprawiono odświeżanie
aktywności pola PHZ seed po zmianie formatu. C++: 10 147 asercji / 3 przypadki;
pełne CTest 10/10 pass. Oryginalny UVtools: pełne maski i cztery pary
Roundtrip zgodne we wszystkich 42 polach globalnych i polach warstw;
niezależny dekoder potwierdza kanoniczne PNG po kwantyzacji i identyczne RLE.
GTK zaszyfrowany eksport, popup/sufiks i Full Open 6 warstw pass.
Konwersja SL1 UVtools gubi light-off; AAInfo=8 wobec natywnego 1 oraz surowe
różnice objętości/czasu pozostają jawnie w raportach, bez osłabiania porównań.
Świeży audyt: 67 ID, 51 zapisanych, 49 pełnych odczytów, dwa znane wyjątki,
16 blocked. To nie kończy wszystkich formatów/wersji ani E3–E6.
[Raport, ograniczenia i artefakty](PHZ_2026-10-05.md). Główny cel aktywny.

## Rodzina LGS, 2026-10-05

Cztery puste writery zastąpiono natywnym LGSFile: nibble RLE/grayscale,
obrót 4K (modele 4000/4500), RGB565 BE, dodatkowy PNG LGS120, rzeczywista
geometria i obsługiwane ekspozycje/odpoczynek/ruch. Usunięto nieużywany
błędny szkic RGB565. Walidacja ograniczeń działa przed slicingiem/eksportem.
117 200 asercji / 2 przypadki i CTest 9/9 pass; oryginalny Roundtrip
pięciu modeli: 42 pola globalne i wszystkie warstwy zgodne, całe RLE
identyczne z UVtools. Niezależne PNG/RLE/obrót/miniatury/header pass.
Zwykły UVtools SL1 Convert zmienia geometrię ekranu: raport pozostaje failed,
bitmapy/RLE zgodne w osobnej kontroli. Nie skopiowano błędnych wymiarów.
Audyt 67 ID: 55 zapisanych, 53 pełne odczyty, SL1SVG/CXDLPv1 pozostają
wyjątkami, 12 blocked. Natywne importy LGS, kompletne profile/wersje,
firmware oraz E3–E6 nadal otwarte. Główny cel aktywny.
[Raport i dowody](LGS_2026-10-05.md).

## Makerbase GR1 / MDLP, 2026-10-05

Zastąpiono puste writery wspólnym MakerbaseArchive: rzeczywisty framing
MKSDLP, RGB565 BE miniatur, UTF-16BE wymiarów i binarne odcinki pionowe.
MDLP ma krótsze ustawienia i nie udaje obsługi pól GR1. Walidacja nie
pozwala po cichu zgubić ruchu/PWM/odpoczynku, przejść ani zmiennego Z.
105 536 asercji / 2 przypadki i CTest 9/9 pass. Cztery pary oryginalnego
Roundtrip (oba formaty × orientacje): 42 pola globalne i wszystkie warstwy
zgodne; piksele względem PNG po progu 128 i pakiety linii identyczne.
Zwykły Convert MDLP także zgodny; GR1 w oryginalnym UVtools ma błędny setter
LiftHeight i zmienia ruch/light-off. Jego raport pozostaje failed, bez
normalizacji ani rozluźniania weryfikacji. Świeży audyt: 67 ID, 57 zapisanych,
55 pełnych odczytów, dwa dotychczasowe wyjątki, 10 blocked. Natywne importy,
pełna biblioteka/wersje, ustawienia UI i E3–E6 pozostają w kolejce.
[Raport i dowody](MAKERBASE_2026-10-05.md). Główny cel aktywny.

## QDT, 2026-10-05

Zastąpiono pusty Emake3DGalaxyArchive natywnym zapisem ścieżek lasera:
oba sposoby współrzędnych X, względne Y, podgląd z interpolacji szarości,
stała geometria i jawna walidacja parametrów nieobecnych w kontenerze.
4 017 118 asercji / 1 przypadek, CTest 9/9 pass. Trzy pary oryginalnego
Full Open/Encode/Convert: 42 pola globalne i wszystkie warstwy zgodne.
Niezależne PNG/piksele i surowe ścieżki główne identyczne; podglądy zgodne
z oryginalnym Convert z SL1. GTK eksport/sufiksy/popup pass, UVtools
odczytał sześć niepustych warstw z GUI. Świeży audyt: 67 ID, 58 zapisanych,
56 pełnych odczytów, dwa znane wyjątki, 9 blocked. QDT nie przechowuje
ruchu/ekspozycji: ograniczenia wynikają z interpretacji UVtools, nie
potwierdzenia firmware. Pozostałe formaty, importy i E3–E6 nadal otwarte.
[Raport i dowody](QDT_2026-10-05.md). Główny cel aktywny.

## VDT v1, 2026-10-05

Właściwy VoxeldanceTangoArchive ma natywny raster, JSON i niezmienione
szare PNG. Zapisuje zmienny Z, ruch dwuetapowy, osobne dolne/normalne PWM
oraz oczekiwania. Statystyki z rzeczywistych PNG/Z; powtarzane maski nie
są ponownie rozpakowywane. Dwa poprawne podglądy FLT, pozostałe kamery
nadal otwarte. 128 asercji pass; trzy pary oryginalnego Roundtrip zgodne
we wszystkich 42 polach globalnych/warstwach/pikselach. Niezależne
maski/manifest/podglądy oraz objętość/masa/koszt i przejścia pass. GTK
sufiksy/popup/eksport pass, UVtools odczytał sześć niepustych warstw.
Zwykły Convert SL1 różni się AA=8/16; raport pozostaje failed. Surowe
JSON różnią się także pochodzeniem/statystykami (UVtools zeruje volume
przy Roundtrip). Różnic nie kopiujemy ani nie ukrywamy. Audyt: 67 ID,
59 zapisanych, 57 pełnych odczytów, dwa znane wyjątki, 8 blocked.
[Zakres, różnice i dowody](VDT_2026-10-05.md). Pełny cel nadal aktywny.

## JXS, 2026-10-05

Zastąpiono pusty UniformationGKoneArchive natywnym JXS: pełna szarość
w pakowanych RGB PNG, prawidłowa geometria, zmienny Z, dwuetapowy ruch,
PWM, odpoczynki, bazowanie i parkowanie. 68 asercji pass; pięć pełnych
odczytów i niezależne wszystkie maski/parametry warstw pass. Cała lista
58 działań identyczna z oryginalnym generatorem w pamięci. Surowy encoder
UVtools zapisuje niewłaściwy control.json; porównania failed zachowano.
Jego globalne dolne/przejściowe warstwy i dolny lift są również błędne,
więc nie deklarujemy pełnej zgodności globalnej. GTK eksport/sufiksy/popup
pass, UVtools odczytał sześć niepustych warstw. CTest 9/9 pass. Audyt:
67 ID, 60 zapisanych, 58 pełnych odczytów, dwa znane wyjątki, 7 blocked.
[Zakres i dowody](JXS_2026-10-05.md). Importy/firmware i E3–E6 otwarte;
główny cel aktywny.

## ZCodex, 2026-10-05

Natywny writer zastępuje pusty ZCodexArchive: szare PNG, prawidłowa
geometria Inkspire, zmienny Z, względny ruch, PWM i jawne ograniczenia
kontenera. Statystyki z rzeczywistych masek/grubości, poprawny podgląd.
70 asercji pass, sześć pełnych odczytów i niezależne maski/metadane/ruch
pass. Dwie pary oryginalnego Roundtrip: 42 pola globalne/warstwy zgodne;
sekwencja bajtowo identyczna także z Convert SL1. Convert różni się
AA/nazwą/czasem; oryginalny Encode gubi custom PWM. Raporty failed
zachowano. GTK eksport/sufiksy/popup pass, sześć niepustych warstw
w UVtools. CTest 9/9 pass. Audyt: 67 ID, 61 zapisanych, 59 pełnych
odczytów, dwa znane wyjątki, 6 blocked. Walidacja JXS/VDT/ZCodex
zwraca błędy do GUI. [Zakres i dowody](ZCODEX_2026-10-05.md).
ZCode RSA/XML, importy i E3–E6 nadal otwarte; główny cel aktywny.

## ZCode — etap RSA, 2026-10-05

ZCodeRSA.hpp: natywny CRT/raw RSA/base64, dokładny klucz i padding
lokalnego UVtools, deterministyczne równoległe kodowanie. 14 asercji
i CTest 9/9 pass. Oryginalna biblioteka potwierdziła wszystkie bajty
20 000 linii i ich odczyt, plus siedem wektorów granicznych/Unicode.
Sam etap szyfrowania: 2,53→0,85 s przy czterech workerach (~2,99×).
Writer całego ZCode nadal blocked: XML/PNG/preview/protokół ruchu
i pełny odczyt/GUI wymagają podłączenia i testów. Licznik formatów
pozostaje 61 zapisanych / 59 pełnych odczytów / 6 blocked z 67 ID.
[Zakres i dowody](ZCODE_RSA_2026-10-05.md). Główny cel aktywny.

## ZCode — pełny native writer, 2026-10-05

Natywny ZCodeArchive: task.xml, pełne szare PNG, RSA, absolutny ruch
w cm/min, dwuetapowy lift/retract, osobne PWM/odpoczynki/przejścia,
bazowanie i parkowanie. Poprawna geometria ze wspólnego pixel_size,
w tym zaokrąglenia CL-92; walidacja Z chroni limit parkowania.
218 asercji i CTest 9/9 pass. Dziesięć pełnych odczytów i niezależne
maski/XML/RSA/ruch/statystyki/preview pass. Podstawowe trzy sekwencje
lcd.gcode bajtowo identyczne z UVtools. Oryginalny dekoder ma błędy
globalne przy pustej warstwie/przejściu/customPWM, a Convert gubi
pixel_size; raporty failed zachowano. PWM0 z dodatnim cure jawnie
odrzucane po wykrytym scalaniu waits. GTK eksport/sufiksy/popup pass;
sześć niepustych masek GUI i Roundtrip: 42 pola globalne/warstwy zgodne.
Audyt: 67 ID, 62 zapisanych, 60 pełnych odczytów, dwa wyjątki, 5 blocked.
[Zakres i dowody](ZCODE_2026-10-05.md). Importy, corner cases, firmware
i E3–E6 nadal otwarte; pełny refaktor aktywny.

## NanoDLP — etap schematu i masek, 2026-10-05

Naprawiono geometrię/orientację/odbicia rastera NanoDLP, wdrożono natywne
kodowanie pełnej szarości (packed RGB, PNG8 i pełny RGB), port schematu
manifestów oraz jawny błąd zamiast pustego sukcesu export_print.
60 asercji kodowania i 192 geometrii pass; Pillow i oryginalny dekoder
potwierdzają cztery maski bajtowo. Testowe archiwa stworzył harness,
nie writer aplikacji. NanoDLP pozostaje blocked: rzeczywiste manifesty,
ruch/statystyki/wersje/GUI wymagają wdrożenia. Licznik ostatniego pełnego
audytu nie zmienia się: 67 ID / 62 written / 60 Full Open / 5 blocked.
[Dowody i następna partia](NANODLP_ENCODER_2026-10-05.md).
Pełny refaktor, w tym UI/UX i E3–E6, nadal aktywny.

## NanoDLP — statystyki konturów i info.json, 2026-10-05

Natywne drzewo konturów (otwory i wyspy, połączenia 8/4), pola konturów,
liczba rodzin, bbox i pixel count. Workspace ograniczony do ROI; czarne
warstwy bez alokacji konturów. layer_info mapuje pola na mm² z dokładną
arytmetyką i serializacją float oryginalnego UVtools. 68 056 masek:
wszystkie 3×3/4×4, 2000 losowych, nesting i siedem pixel_area; 12 pól
w każdym przypadku zgodnych bez tolerancji. Zachowano wcześniejszy
failed raport dużej powierzchni, poprawiono dzielenie float zamiast
promocji do double. Writer nadal blocked — nie doliczamy testowych
masek/statystyk do działających eksportów. Ruch, manifesty, wersje
oraz ZIP/GUI i E3–E6 pozostają otwarte.
[Dowody](NANODLP_CONTOURS_2026-10-05.md). Główny cel aktywny.

## NanoDLP — pakiety i manifesty, 2026-10-05

Natywny encoder cache'uje statystyki obok PNG; przygotowanie manifestów
nie wymaga ponownego dekodowania packed RGB. Mapped geometry/pitch,
obsługiwane cure/waits/lift/PWM, provenance, bbox i rzeczywisty wolumen.
Osiem decoder-only fixture'ów/24 maski: Pillow i original Full Open
zgodne dla sprawdzanego zakresu. ZIP utworzył harness, nie writer app.
Skrypty FSS wyczyszczone — ruch i pozostałe parametry wymagają realizacji;
export_print/rejestr nadal blocked. V1/V2, RGB suffix, variableZ,
preview/mesh, memory budget i GUI nadal otwarte. Audyt bez zmiany:
67 ID / 62 written / 60 Full Open / dwa wyjątki / 5 blocked.
[Dowody i ograniczenia](NANODLP_METADATA_2026-10-05.md). Cel aktywny.

## NanoDLP — korekta jednostek Plate.TotalSolidArea, 2026-10-05

Changelog v5.2.1 #1069 wskazuje mm², ale aktualny setter MaterialMilliliters
UVtools nadal zapisuje ml. Zwykły Roundtrip zachowuje poprawione 0.18;
jawne ustawienie ml przez oryginalne API zmienia pole na 0/0.123 przy
niezmienionym info sum=0.18. Surowy failed zachowano. Nasze przygotowanie
manifestów używa teraz sumy info mm²; objętość jest oddzielną wewnętrzną
statystyką. Definicja agregatu w firmware nadal wymaga potwierdzenia.
312 asercji i 8 fixture'ów/24 maski pass; writer/GUI NanoDLP nadal blocked.
[Dowody i zakres](NANODLP_UNITS_2026-10-05.md). Pełny refaktor aktywny.

## FDG v2 — 2026-10-05

Natywny FDGArchive zastępuje uszkodzony writer: nagłówek 216 B, preview,
pełne warstwy i adresowanie, ruch/light-off, variable Z, statystyki, dedup
oraz XOR. Format odblokowano w GUI; GUI zapisuje plaintext, profilowy seed
FDG jest jeszcze do podłączenia. 421 asercji / 2 przypadki pass; obie
orientacje i seed=0/UINT32_MAX, 12 niezależnych wektorów oryginału.
Full Open i oryginalny Roundtrip: cztery pliki, wszystkie maski/RLE,
42 pola globalne i warstwy zgodne. GTK eksport/sufiksy/popup pass;
sześć niepustych masek GUI i osobny Full Open/Roundtrip zgodne.
Surowy Convert UVJ→FDG nadal ma failed różnice metadanych/rest/czasu;
wszystkie odszyfrowane RLE obu orientacji są identyczne. Raporty zachowano.
UVtools nie zachowuje poprawnej objętości nagłówka przy Roundtrip variableZ;
nasz writer liczy ją z faktycznych Z i zajętych pikseli. Różne PWM, import,
precyzja parametrów, >4 GiB i firmware pozostają do odbioru.
Świeży audyt: 67 ID / 63 written / 61 Full Open / dwa wyjątki / 4 blocked:
CBT, encrypted CTB v5, SVGX i NanoDLP. Brakujące rozszerzenia i wersje oraz
E3–E6 nadal otwarte. [Dowody i ograniczenia](FDG_2026-10-05.md).
Pełny refactor nadal aktywny; checkpoint w build/fdg-2026-10-05/checkpoint/.

Końcowy CTest tej partii: 9/9 pass, 66,65 s; libseqarrange_tests
pominięto jako niezmieniony (wcześniej pass w PHZ).

## CTB v1 / migracja błędnego CBT — 2026-10-05

`.cbt` nie jest rozszerzeniem rejestrowanym przez UVtools. Wpis zastąpiono
jawnym ctb_v1 / .v1.ctb, z migracją starych ID w presetach i fabryce.
Writer zapisuje prawdziwy Version=1, gray7/RLE, geometrię, variable Z,
przejścia, PWM i light-off; nie podmienia pliku na CTB v2. V1 pomija blok
PrintParameters. Niestandardowe ruchy są jawnie odrzucane zamiast tracone;
domyślne wartości dekodera nie są certyfikatem firmware drukarki.
112 asercji, dwie orientacje, maski i wszystkie odszyfrowane RLE zgodne
z oryginałem. Full Open/Roundtrip: 42 pola globalne/warstwy bez różnic;
również GTK eksport/sufiksy/popup i sześć niepustych masek GUI pass.
Surowy Convert nadal failed dla light-off/MachineZ/czasu; raport zachowano.
Końcowy CTest 9/9 pass, 77.88 s; libseqarrange_tests niezmieniony, pominięty.
Świeży audyt: 67 ID / 64 written / 62 Full Open / dwa wyjątki / 3 blocked:
encrypted CTB v5, SVGX i NanoDLP. Brakujące wersje/rozszerzenia/importy,
firmware, profile i UI/UX oraz E3–E6 pozostają otwarte.
[Dowody i ograniczenia](CTB_V1_2026-10-05.md).
Checkpoint: build/ctb-v1-2026-10-05/checkpoint/manifest.json. Cel aktywny.

## SVGX — wspólne ścieżki konturów, 2026-10-05

Skorygowano założenie ZIP: SVGX ma binarny header, BMP i jeden SVG.
Wyodrębniono PixelContourTree z NanoDLP, z opcjonalnymi CHAIN_APPROX_SIMPLE
points, rodzicami/root, otworami/wyspami oraz area/perimeter. Stats-only
nie alokuje path storage. Oryginalny UVtools/OpenCV: 68 056 masek,
127 792 kontury i 641 152 wierzchołki zgodne; DrawContours z natywnych
punktów odtwarza każdy piksel. Dotychczasowe 12 pól NanoDLP nadal zgodne.
604 213 asercji / dwa przypadki pass; 224 asercje geometrii siedmiu
rodzin obejmują także SVGX z prostokątnymi pikselami/orientacją/odbiciami.
Usunięto błędną, nieużywaną klasę ZIP; bezpośredni SVGX export_print
przestał zwracać pusty sukces. Registry i codec/writer nadal blocked.
Original serializer potwierdza header=28/BMP=54 i niespójność deklaracji
UTF-16 z zapisem UTF-8. Header/BMP/XML, konwersja px↔mm, parametry,
pełny plik/UVtools/GUI i limity worker pamięci pozostają do wdrożenia.
Końcowy CTest: 9/9 pass, 66.08 s; libseqarrange_tests niezmieniony,
pominięty (wcześniej pass w PHZ). Nie zaliczamy konturów jako działającego
eksportu SVGX. Ostatni pełny audyt bez zmiany: 67 ID / 64 written /
62 Full Open / dwa wyjątki / 3 blocked. E1/E2 oraz E3–E6 nadal otwarte.
[Dowody i następna partia](SVGX_CONTOURS_2026-10-05.md).
Checkpoint: build/svgx-2026-10-05/contours-checkpoint/manifest.json.
Główny cel pozostaje aktywny.

## SVGX — pełny writer i eksport GUI, 2026-10-05

Natywny header/BMP/SVG i pakiet konturów wdrożone. Full Open: 48 masek
bez różnic; GUI i original roundtrip: 42 globalne pola i sześć niepustych
masek zgodne. Odblokowano SVGX z walidacją możliwości formatu. Audyt:
67 ID / 65 written / 63 Full Open / dwa wyjątki / 2 blocked (encrypted CTB v5,
NanoDLP). Pozostałe wersje/rozszerzenia/importy i E3–E6 nadal otwarte.
[Dowody, ograniczenia i zachowane błędy referencji](SVGX_FILE_2026-10-05.md).
Główny cel aktywny. Checkpoint: build/svgx-2026-10-05/writer-checkpoint/manifest.json.

## CTBEncrypted v4/v5 — 2026-10-05

Odblokowano samodzielny native writer dla obu wersji, z AES/podpisem,
XOR-RLE, dwufazowym ruchem, zmiennym Z i UTF-8. Osiem plików i dwa
rzeczywiste eksporty GUI przeszły pełny odczyt oraz weryfikację pikseli
i parametrów. Popup i końcówki obu wersji przechodzą test GUI.
Audyt: 68 ID / 67 written / 65 Full Open / dwa wyjątki / jeden blocked
(NanoDLP). Surowe różnice i błędy ponownego zapisu UVtools zachowano.
[Dowody i ograniczenia](CTB_ENCRYPTED_FILE_2026-10-05.md).
Brakujące wersje/importy oraz profile, UI/UX i E3–E6 nadal otwarte.

Końcowe regresje: 1904 asercje / 10 przypadków pass; CTest 9/9 pass
(32.15 s). Niezmieniony libseqarrange_tests pominięto; przeszedł wcześniej.
Checkpoint: `build/ctb-encrypted-2026-10-05/checkpoint/manifest.json`.
Główny cel pozostaje aktywny.

## NanoDLP — API z jawnym profilem sterownika, 2026-10-05

Dodano kompletny natywny kontener v1/v2 Mono/RGB, manifesty, podgląd
i szablony przekazanego profilu. 16 plików / 48 masek przechodzi
oryginalny Full Open, niezależną weryfikację i ścisły Roundtrip.
Eksport z GUI pozostaje blocked: profile sterownika i parametry ruchu
wymagają powiązania. API nie wykonuje i nie certyfikuje skryptów.
[Dowody, surowe różnice i kolejka](NANODLP_PROFILE_WRITER_2026-10-05.md).
Pełny refactor i niezmieniony audyt rejestru pozostają otwarte.

Końcowe testy: 960 asercji / 2 przypadki pass; CTest 9/9 pass (32.35 s).
Niezmieniony libseqarrange_tests pominięty, z wcześniejszym wynikiem pass.
Końcowe fixtures: `profile-verified-fixtures`, verifier: `profile-verified-independent.json`.
Checkpoint: `build/nanodlp-2026-10-05/profile-checkpoint/manifest.json`.
Feedback i trzy pliki testexports użytkownika zachowane; cel pozostaje aktywny.

## NanoDLP — preset i GUI, 2026-10-06

Profil kontrolera jest zapisany wewnątrz presetu, importowany i walidowany
z GUI. Mono/RGB v1/v2 są dostępne w zwykłym eksporcie. Poprawiono wybór
najdokładniejszej końcówki compound; .nanodlp zachowuje wariant presetu.
Naprawiono modalne zdarzenia GTK oraz czas życia bufora ścieżki importu.
Import, błędny JSON, zapis i restart przeszły, podobnie cztery eksporty GUI,
zmiana rozszerzenia i stabilność listy. Cztery ponowne zapisy w UVtools:
42 pola globalne i wszystkie 24 maski bez różnic.

Świeży audyt: 71 wariantów zapisanych, 69 pełnych odczytów, dwa znane
wyjątki SL1SVG/CXDLP v1, zero blokad writera. To nadal nie wszystkie
wersje/rozszerzenia/importery UVtools. Brakuje m.in. CTB v3/v4/v5/GKtwo,
części obrazów i SR, pełnych natywnych importerów, następnie E3–E6.
NanoDLP: ruch i prędkości pozostają własnością jawnego profilu kontrolera,
natywny import, model 3D, czas i sprzętowy odbiór pozostają otwarte.
Szczegóły: [NanoDLP GUI](NANODLP_GUI_2026-10-06.md).

## CTB v3/v4/v5 i GKtwo — 2026-10-06

Natywne writery i cztery ID presetów, pełne metadata i per-layer motion
według wersji, dwa podglądy i oddzielny magic GKtwo. Walidacja przed slicingiem
i wybór compound końcówki są podłączone. 8988 asercji/12 przypadków, CTest9/9;
16 plików/64 maski, niezależne pixels/parametry i prawdziwe volume/mass/cost.
Cztery testy GUI eksportu/list/rozszerzeń oraz 42 pola/24 warstwy i osiem
podglądów po oryginalnym Roundtrip przechodzą. Surowe różnice UTF-8 i Convert
pozostają failed i mają wyjaśnione źródła; nie zaliczamy przez ich ignorowanie.

Świeży audyt: 75 zapisanych, 73 Full Open, znane wyjątki SL1SVG/CXDLP v1.
Nadal do pracy: warianty/AA CBDDLP i Photon, 11 rozszerzeń ImageFile, natywne
importery, ustawienia seed/per-layer w GUI i sprzętowy odbiór. E3–E6 pozostają
otwarte. Obraz jednowarstwowy nie może cicho zastąpić całego zadania druku.
Szczegóły: [CTB modern](CTB_MODERN_2026-10-06.md).


## ImageFile i eksport masek — 2026-10-06

Jedenaście rozszerzeń obrazów ma natywne kodeki, import i zapis z edytora.
Eksport bieżącej maski/zakresu jest jawny i oddzielony od kontenerów drukarki;
zakres jest publikowany atomowo do nowego folderu. Auto-końcówka, Cancel między
warstwami, ochrona istniejących folderów i zachowanie dirty/Undo są sprawdzane.
PBM: niezerowe → białe zgodnie z UVtools, podgląd kwantyzuje od razu. PNM:
Maxval ASCII/binary i 16-bit poprawiono po oryginalnych próbach. SR/RAS mają
jawną paletę, bo oryginalny grayscale writer/reader UVtools daje czarne maski.
Surowe raporty tych różnic pozostają failed; osobne referencje RGB UVtools
potwierdzają poprawne piksele. JPEG jest jawnie stratny; JP2 bezstratny.

CTest 9/9 (35.84 s); 6 004 227 asercji/63 regresje; 66 asercji importu
referencji; dwa scenariusze GUI i dziewięć plików Full Open pass, zero prób
połączeń w badanych scenariuszach. Audyt kontenerów: 75 written / 73 Full Open,
znane wyjątki SL1SVG/CXDLP v1. Nie oznacza to całego E2 jako zakończonego:
multipage TIFF, zaawansowane kolory/16-bit/CMYK/JP2, pakiet czystego systemu
oraz pozostałe wersje/AA i importery nadal otwarte. E3–E6 pozostają w kolejce.
[Zakres, różnice i dowody](IMAGE_FILE_2026-10-06.md).

### M2.3b.5 — CTB v3 nominal defaults and conversion (2026-10-11)

REVIEW: runtimeRED32/80 plusselfRED2/152 → focusedGREEN480/1; native getter, neutral absent-phase/wait semantics and self-overlay guards implemented. Final regression, independent UVtools readback and GUI acceptance pending. [Report](M2_CTB_V3_GLOBALS_2026-10-11.md). Full conversion/fresh document/all-format goals remain open.

### M2.3b.5 checkpoint po ochronie publicznego overlay

Podstawowa macierz v3→v3/CTB4/5/AES4/5 oraz source-ownership guard odebrane. Native 660/1, regresja 17222/58, CTest 9/9 (69.22 s); independent 40 plików/120 warstw; GUI 2 + FullOpen 6/18, zero strat raw, 0 INET w dwóch audytowanych sesjach. Final identity `identity-overlay-final.json`. Dodatkowy RED 100/660 przed poprawką zachowany.

Otwarte: fractional native defaults, pełniejsze flag0/metadata/capabilities, pozostałe rodziny, sprzęt. M2.4 READY: świeże parametry wymagają jawnego pochodzenia i kompletnej ścieżki project/export zanim zostaną udostępnione w GUI. Pełny goal pozostaje aktywny.


### Aktualny checkpoint wdrożenia M2 (2026-10-13)

Aktualną kolejność, zależności i stan kart prowadzi [kanban wdrożenia](KANBAN_WDROZENIA_2026-10-11.md). Wcześniejsze wpisy REVIEW/READY powyżej są historyczne; nie należy traktować ich jako aktualnego odbioru.

Odebrane zakresy: fresh descriptor/project v3, eksport owned CTB4/5 i AES4/5, pure builder producentów; generated CXDLPv4 TOTAL/FIRST oraz complete time z 300 s preparation. Wdrożona poprawka fallback prędkości CXDLPv4 jest w REVIEW: 768 assertions/48 wariantów, regresja 56709/103, build aplikacji PASS; niezależny odczyt i CTest w toku. [Raport fallback](M2_CXDLPV4_FALLBACK_2026-10-13.md).

P0 otwarte: pełne kontrakty fresh SL1/CXDLP, wiring ModelSlice/Calibration/PCB, statystyki na owned rows, GUI parametrów z historią i restartem, bezstratny preflight wszystkich formatów. Kolejne cele obejmują kompletną funkcjonalność UVtools, profile/UI, wydajność, cancellation oraz samodzielną dystrybucję offline. Powyższe odbiory backendu nie oznaczają pełnego odbioru workflow GUI ani wszystkich drukarek.

Odbiór fallback CXDLPv4: independent48/144 i CTest9/9 (47.22s) PASS. Karta DONE w ograniczonym zakresie. Aktualna praca: typowany raw row reader CXDLPv4, jako fundament kompletnego kontraktu; globals/planner/export/GUI nadal otwarte.


M2.4b.5a odebrana jako read-only native row helper:14pól, raw TOTAL→FIRST i zachowanie `.004`/zero speeds, bounds/phase/PWM guards. RuntimeRED64/96 → GREEN160/1; regression56869/104,CTest9/9 (47.37s),independent16files/48layers/4SHA PASS. [Raport](M2_CXDLPV4_PARAMETERS_2026-10-13.md). Następna karta5b: native globals/provenance; publiczny import nadal mask-only, bez udostępnienia niezachowywalnych parametrów w GUI.


### M2.4b.5b native CXDLPv4 globals — REVIEW (2026-10-13)

Wdrożono native snapshot13normal/7bottom oraz readonly getter w LayerDocument. Sześć bottom globals bez osobnych slotów pozostaje absent; rawuint32 counts, unknown flags i nieautorytatywne mirrors zachowane oddzielnie. RuntimeRED128/288 → GREEN1264/1; app/testbuild PASS, regression58133/105 i CTest9/9 (47.29s) PASS. Testy obejmują malformed metadata, getterimmutability przy paint oraz projectrestart bez zmiany pikseli/metadanych. Independent16realexports48layers i35diagnostic headerprobe nadal w odbiorze. [Raport](M2_CXDLPV4_GLOBALS_2026-10-13.md).

Następne5c: effective bottom provenance z aktualnych masek/exposure/dummy selection, potem pełny fresh planner/write/history/GUI. Getter powinien być pobrany raz/cache przy podłączeniu GUI, aby nie walidować CRC/layout każdego pola. Nie ma nowych editable capabilities ani deklaracji kompletnego fresh CXDLP/SL1.


Odbiór5b readonly nativeglobals: independent16realexports48layers +51actualheaderinputs (35diagnostic21reject/14retain),7SHA PASS. Karta5b DONE wyłącznie jako introspection. Nowa P0 5d:16exportedfiles rawtransition5/publicUV0; pierwszy strictfailedreport zachowany. Przed fresh/GUI konieczne testy liczników i ekspozycji w generatedwriter/freshbuilder/targetplan, bez modyfikowania surowego odczytu na wartości publiczne. Full goal pozostaje aktywny.


### M2.4b.5d — generated CXDLPv4 count contract odebrany (2026-10-13)

Naprawiono rozbieżność między zapisanym licznikiem przejść i odczytem UVtools. Metadane generowanego pliku wynikają z rzeczywistych warstw i publicznej projekcji ekspozycji; oryginalny mianownik, konfiguracja i obrazy pozostają zachowane. Granice uint16 są sprawdzane przed publikacją. GREEN1580/3, regression59713/108, CTest9/9 (52.27 s), linked app/test build PASS. Independent64files/412layers,5SHA i binarydiff64par PASS. [Raport](M2_CXDLPV4_COUNTS_2026-10-13.md).

Aktualnie przygotowywana następna P0: M2.4b.5c, effective bottom provenance z aktualnych masek i ekspozycji. Pełny świeży eksport SL1/CXDLP, wiring producentów, statystyki, GUI/history/project restart i wszystkie formaty nadal wymagają realizacji; aktualny stan i zależności są w kanbanie. Odbiór generated metadata nie zamyka pełnego celu.


### M2.4b.5c — readonly effective bottom provenance odebrane (2026-10-13)

Nowy odczyt oddziela native7 bottom od sześciu ustawień wyliczanych według UVtools i wskazuje pierwszą aktywną warstwę. Aktualne obrazy, exposure threshold, grayscale quantization, Undo/Redo i restart projektu działają; wynik nie zmienia źródłowych parametrów. GREEN831/2, regression60488/110, CTest9/9 (63.44 s), app/test build PASS. Independent40files120layers/40sidecars,7SHA i8nativepreservationpairs PASS. [Raport](M2_CXDLPV4_EFFECTIVE_2026-10-13.md).

Następna P0: M2.4b.5e typed owned fresh CXDLPv4 plan/writer, z konkretnymi odmowami strat nieprzedstawialnych bottomglobals/counts/masks. Parent fresh SL1/CXDLP, native editor persistence/history/GUI, ModelSlice/Calibration/PCB, owned statistics i wszystkie formaty pozostają obowiązkowe. Dokumenty kanbanu wskazują zależności i dalszą kolejność; odczyt diagnostyczny nie oznacza gotowego edytora parametrów.
## Koordynowane wznowienie — 2026-10-15, odbiór bieżącego etapu

- Wszyscy trzej subagenci pracowali nad oddzielnymi pionami; root wykonał
  integrację i kolejkę buildów/testów. [Raport zbiorczy](M2_COORDINATED_REFACTOR_2026-10-15.md)
  i [bieżący kanban](KANBAN_WDROZENIA_2026-10-11.md).
- P0 UX: pędzel/gumka, wskazanie zakresu i wybór warstw nad podglądem.
  FinalGUI real drag2–4/UndoRedo/Apply/3MF/restart/eksport PASS; UVtools1CXDLPv4/
  6rekordów PASS;2procesy/network0. Kontrola aktualności launchera PASS.
  Małe okna/DPI i pełny workspace nadal otwarte.
- P0 slicing: zadania półwyspów per część. Ramka29,241→27,687s (~5,3% w jednej
  parze), faza6,241→4,810s; eksport2,897s.213RLE i wszystkie parametry zgodne;
  FullOpen213rekordów/42pola globalne bez różnic. Offsetreuse0/3029trafień nie
  przyspieszył tego modelu. Dalsza wydajność i macierz modeli nadal otwarte.
- V22/V16: pełny pattern/layout/finishing recipe v3 i legacy1/2;38hostile,
  roundtrip/snapshot/edited masks i real generator PASS. FinalGUI/restart +
  UVtools3UVJ/33rekordy PASS;2procesy/network0. Podgląd read-only, pełny formularz
  tworzenia pozostaje do wdrożenia według M2_EXPOSURE_FORM_NEXT_2026-10-15.md.
- Build app/test0, focused5623/19 i reference1492/4 PASS; CTest10/10,74,98s.
  Zamrożone tożsamości21plików i chronione SHA użytkownika zgodne.
  Dowody: `build/coordinated-refactor-2026-10-15/final-acceptance.json`.
- Pełny goal, wszystkie formaty/drukarki, profile, PCB, reszta UVtools i niezależna
  dystrybucja pozostają otwarte. Nie deklarujemy kompletnego refaktoru ani certyfikacji sprzętu.

## W toku — formularz ExposureFinder i następne bramki P0, 2026-10-15

- Trzech subagentów przygotowało pełny formularz, parser/preflight i wspólny
  model zadań. Root integruje CMake/menu i testy rzeczywistego GUI. Pierwszy
  wspólny build nadal pracuje; nowe funkcje nie mają jeszcze odbioru runtime.
  [Bieżący raport](M2_EXPOSURE_FORM_IMPLEMENTATION_2026-10-15.md).
- Test bazowego binarium: RED — brak wejścia ExposureFinder calibration.
  Nowe testy GUI obejmują Generowane/Manual, plan, normalizację, publikację,
  anulowanie i zachowanie szkicu; weryfikatory porównają rzeczywiste eksporty
  UVJ z projektami i oryginalnym UVtools. Składnia Python PASS, wykonanie pending.
- Test launchera PASS po objęciu kontrolą aktualności również nowego formularza,
  MainFrame i helpera zadań. To nadal kontrola wskazanych źródeł GUI.
- NOWE P0 z audytu kodu: możliwa utrata niskich jasności w kalibracji dla
  pm4u/pm7/pm7m/pwsz. Wymagany test wykonawczy i oddzielenie palety natywnej
  od dostępności owned parameter route. [Plan](M2_FORMAT_NEXT_GATE_2026-10-15.md).
- Zależności FFF nadal istnieją w wariancie SLA. Dowody symboli i konkretne
  kolejne cięcie: [odłączenie emitera](M2_FFF_COMPILE_DEPENDENCY_CUT_2026-10-15.md).
  Ukrycie GUI nie stanowi usunięcia tych zależności.

