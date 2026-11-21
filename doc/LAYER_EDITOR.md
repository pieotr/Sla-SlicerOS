# Edytor warstw SLA

Aktualizacja 2026-11-07: bieżące instrukcje pracy w widoku **Sliced**, pędzla,
gumki, zakresów i podglądu znajdują się w [manual.txt](../manual.txt).
Poniżej zachowano również historyczne etapy rozwoju; opis „pierwszego
działającego zakresu” i jego ograniczeń nie jest aktualną macierzą formatów.
Znane ograniczenia wydania: [CHANGELOG](../CHANGELOG.md).

W menu Window → SL1 Layer Editor (Ctrl+Shift+L) otwórz plik `.sl1` lub `.sl1s` zawierający
warstwy PNG. To osobne okno pracy nad zeslicowanym archiwum. Aby edytować
bieżący model 3D, zakończ slicing i użyj przycisku Edit layers pod eksportem.
Nie trzeba wcześniej zapisywać pliku. Edytor korzysta z masek zachowanych
podczas slicingu, bez tworzenia tymczasowego SL1 i bez ponownej rasteryzacji.
Apply to sliced result przenosi edycje do wyniku dostępnego w głównym oknie.
Export edited layers zapisuje wynik wybranym dostępnym kodekiem. Dla osobno
otwartego archiwum SL1 pozostaje Save as SL1. Warstwy wektorowe SL1SVG
nie udostępniają edycji pikseli.

- Layer wybiera warstwę, licząc od 1.
- Radius (pixels) ustala promień okrągłego śladu w pikselach oryginalnej maski.
- Grayscale / threshold ustala jasność: 0 usuwa naświetlanie, 255 daje pełne.
- Przeciąganie po podglądzie rysuje ciągłą linię; całe pociągnięcie
  tworzy jeden krok Undo. Obraz jest skalowany do okna;
  współrzędne kliknięcia są przeliczane na oryginalną rozdzielczość.
- Threshold layer zamienia aktywną warstwę na czerń/biel według podanego progu.
- Invert layer odwraca polaryzację aktywnej maski: 0 ↔ 255, 127 ↔ 128.
  Zachowuje odcienie szarości i tworzy jeden krok historii.
- Undo/Redo cofa i ponawia operacje także po zmianie wyświetlanej warstwy.
- Zoom Fit / 25–400% zmienia wyłącznie podgląd; powiększony obraz jest
  przewijany. Promień pędzla pozostaje w pikselach oryginalnego rastra.
- Range wskazuje domknięty zakres numerów warstw. Apply to range wykonuje
  progowanie, invert, odbicie poziome/pionowe, obrót 180° lub wypełnienie.
  Całość to jeden Undo; anulowanie/błąd pozostawia maski i historię bez zmian.
- Ctrl+Z / Ctrl+Y odpowiadają Undo / Redo.
- Save as SL1 zapisuje edytowane warstwy. Ponowne otwarcie odtwarza ich piksele.
- Escape podczas pociągnięcia anuluje tylko tę niezakończoną kreskę, zachowując
  wcześniejsze edycje, historię i wybrany zakres. Późniejsze puszczenie myszy
  nie zatwierdza kreski. Poza rysowaniem Escape zachowuje akcję wyjścia;
  odrzucenie niezapisanych zmian wymaga potwierdzenia.

Współrzędne edycji odpowiadają zapisanemu PNG (początek w lewym górnym rogu).
Nie wykonuje się ponownie obrotu lub odbicia ustawionego w profilu drukarki.
Nazwy wpisów archiwum i zawartość metadanych, miniatur oraz dodatkowych plików
są zachowywane. Zapisywany jest nowy ZIP; kompresja ZIP nie musi być identyczna.
Plik wyjściowy zastępowany jest dopiero po ukończeniu zapisu pliku tymczasowego.

## Aktualne ograniczenia

Jest to pierwszy działający zakres edytora, nie pełny odpowiednik UVtools.
Obsługuje tylko top-level PNG z numerem warstwy co najmniej pięciocyfrowym,
jak w eksporcie SL1. Brak importu binarnych formatów drukarek, narzędzi
wektorowych, zaznaczeń i edycji
parametrów ekspozycji pojedynczej warstwy. Historia nie jest zapisywana do pliku.

Edytor zachowuje miniatury i statystyki zapisane w źródle SL1; nie przelicza ich po
zmianie maski. W szczególności objętość żywicy i miniatura mogą być nieaktualne.
Operacje nie zmieniają projektu 3D, podpór ani oryginalnego modelu. Zmiana ekspozycji i parametrów ruchu zachowuje zastosowane poprawki.
Zmiana modelu, ekranu, orientacji lub gamma unieważnia maski i usuwa edycje.
Wynik można zachować jako plik drukarki. Zastosowane do wyniku slicingu edycje
zapisują się również w projekcie 3MF; szczegóły przywracania opisano poniżej.

Na razie archiwum mieści się w pamięci w postaci skompresowanych PNG i danych
pozostałych wpisów. Limit odczytu wynosi 1 GiB, pojedynczy obraz do 128 Mi pikseli
oraz do 32768 pikseli w osi. Historia ma do 32 operacji i do 128 MiB
skompresowanych obrazów. Operacja większa niż budżet jest odrzucana; można
zastosować ją do mniejszego zakresu. Podgląd ma limit 32 Mi pikseli/16384 w osi.
Duże dokumenty wymagają w przyszłości magazynu na dysku. Podgląd dekoduje
jedną warstwę i tworzy kopię RGB według zoomu. Zakresy mają postęp i anulowanie
między warstwami i wykonują się w wątku roboczym. Dekodowanie pojedynczego PNG
nie jest przerywane w połowie. Eksport działa w tle z dialogiem oczekiwania.

## Aktualizacja 2026-10-03

Menu Window → Open layer archive otwiera SL1 i UVJ. Schemat rozpoznawany jest po
zawartości; nieznane metadane zachowywane są przy zapisie. UVJ wymaga masek PNG
8-bit grayscale i zgodnego układu/indeksów. Save edited archive zachowuje format;
nie służy do konwersji UVJ na format konkretnej drukarki.

Zakres ma również 7 operacji morfologicznych z kwadratowym jądrem i Reflect101.
Promień pochodzi z pola Radius. Praca odbywa się w wątku roboczym, a postęp i
anulowanie w GUI. Anulowanie jest sprawdzane między wierszami/kolumnami
morfologii oraz między warstwami. Kompresja/dekompresja PNG nie jest przerywana
w połowie. Anulowany zakres nie zmienia dokumentu ani historii.

## Zapis edycji w projekcie 3MF

Apply to sliced result oznacza projekt jako zmieniony. File → Save project
zapisuje zastosowane maski w Metadata/SLA/layers/ oraz manifest layers.json
z wysokościami Z i odciskiem geometrii oraz ustawień rasteryzacji. Zapis jest
atomowy: błąd nie zastępuje poprzedniego projektu częściowym plikiem.

Otwórz 3MF jako projekt i wykonaj Slice now. Przed eksportem wynik otrzyma
zapisane maski, jeśli pasują do modelu, podpór, otworów, ekranu i wysokości Z.
Zmiana ekspozycji lub formatu eksportu nie unieważnia masek. Niezgodny dokument
jest pomijany z komunikatem, a program zachowuje nowy wynik slicingu.
Import modelu do istniejącej sceny oraz import samych ustawień nie przywraca
edycji. Historia Undo nie jest przenoszona między sesjami.

Objętość i czas wyniku slicingu są przeliczane po Apply oraz dla niezależnego
eksportu edytowanych masek. Zapis osobno otwartego archiwum zachowuje nieznane metadane i miniatury;
zużycie materiału SL1 jest przeliczane, gdy plik zawiera pełną geometrię. Edycje nie zmieniają powierzchni modelu 3D.

Eksport edytowanego wyniku slicingu przedstawia pierwszą maskę jako miniaturę
2D z zachowaniem proporcji. Nieedytowany wynik korzysta z renderera 3D.

Przy zapisie osobno otwartego SL1 zużycie żywicy w config.ini i config.json
jest przeliczane z niezerowych pikseli, fizycznej powierzchni ekranu oraz
wysokości pierwszej/normalnej warstwy zapisanych w prusaslicer.ini. Przy
niepełnej geometrii starego pliku program zachowuje jego metadane; nie używa
w ich miejsce aktywnego profilu drukarki. Nieznane wpisy i miniatury są zachowane.

## Pojedyncza maska PNG

Open layer archive przyjmuje również PNG. Dokument ma jedną warstwę bez
przypisanej drukarki i geometrii fizycznej. Szarość 8-bit jest zachowana;
kolor jest przeliczany na luminancję, a alfa nakładana na czerń. Save edited
PNG zapisuje atomowo surowy PNG 8-bit w skali szarości, nie archiwum ZIP.
Domyślna nazwa i końcowa ścieżka otrzymują .png. Błąd zapisu zachowuje
poprzedni plik oraz oznaczenie niezapisanych zmian.

## CWS

Import CWS wymaga manifest.xml, wskazanego profilu SliceBuildConfig i pliku
instrukcji. Kolejność masek pochodzi z tabeli Slices, niezależnie od nazw
oraz kolejności wpisów ZIP. Brakujące lub powtórzone maski i rozdzielczości
niezgodne z profilem są odrzucane. Obsługiwane są maski 8-bit w skali szarości
oraz RGBA w wariancie `.xml.cws`. XML.CWS jest osobnym wyborem formatu
eksportu: zapisuje PNG32 z jednakowymi kanałami RGB i nieprzezroczystą alfą.
Edytor przelicza importowane RGBA na szarość, z alfą na czarnym tle, bez
bufora RGBA dla całego obrazu. Zapis przywraca PNG32 oraz pełne rozszerzenie
`.xml.cws`; zachowuje profil, instrukcje i nieznane wpisy. Import działa w tle.
NovaMaker z `slice.conf` jest osobnym wyborem eksportu (`cwsnovamaker`,
NovaMaker CWS). Import jest rozpoznawany po zawartości, bez manifestu XML.
Maski 8-bit grayscale są porządkowane według indeksów w nazwach, niezależnie
od kolejności w ZIP. Braki, powtórzone indeksy/ustawienia, błędne wymiary,
uszkodzone PNG i brak/niejednoznaczność instrukcji są odrzucane.
Zapis zachowuje `slice.conf`, instrukcje i nieznane wpisy bez modyfikacji.
RGB.CWS jest osobnym wyborem (`cwsrgb`, `.rgb.cws`). Trzy poziome piksele
maski są zapisane jako B/G/R jednego piksela PNG. PNG ma szerokość LCD/3;
`slice.conf` i instrukcje zachowują pełną rozdzielczość LCD. Szerokość
po zastosowaniu orientacji musi być podzielna przez 3; błędna konfiguracja
jest odrzucana, bez zmiany geometrii lub dodawania pikseli.
Import wykrywa pakowanie po zawartości i rozpakowuje pełną maskę z zachowaniem
256 szarości. Edycja/Undo/Redo używają pełnej rozdzielczości; zapis ponownie
pakuje dane i zachowuje pełny suffix `.rgb.cws`, `slice.conf` i instrukcje.
Nie miesza się grayscale i packed RGB w jednym archiwum.
Dotychczasowy wybór
CWS XML (8-bit grayscale) zachowano jako oddzielny schemat `.cws`;
nie potwierdzono zgodności firmware żadnego wariantu.

## Generic ZIP

Oddzielny format eksportu zapisuje manifest.uvtools, maski od indeksu 1
i opcjonalne podglądy. Zawiera fizyczną geometrię ekranu oraz jednakową
wysokość warstwy, bez ekspozycji/ruchu. GUI wyświetla tę informację.
Odczyt maskowy rozpoznaje kontener po zawartości i pilnuje numerycznych,
ciągłych indeksów oraz rozdzielczości. Edycja i zapis zachowują manifest.

## Voxeldance Additive ZIP

Eksporter i edytor używają oddzielnego ID vda oraz oryginalnego schematu
VDARoot z XML i maskami od 1. Edytor zachowuje kolejność tabeli, kontroluje
indeksy, referencje, rozdzielczość, liczbę oraz rosnące Z. Zapis zachowuje
manifest. Eksporter wymaga równych wysokości dla zgodności z inicjalizacją
warstw w UVtools. Ten kontener przenosi geometrię i maski, bez ruchu/ekspozycji.

## Chitubox ZIP

ID chitubox_zip zapisuje maski i natywną sekwencję naświetlania/ruchu SLA.
Odczyt bada run.gcode, kontroluje nagłówek, numeryczne indeksy, liczbę
i wymiary PNG. Zapis zachowuje pozostałe wpisy oraz polecenia. Przy pełnych
komentarzach Z aktualizuje objętość po edycji; masę i koszt wyprowadza
z wcześniejszych proporcji, jeśli są dostępne. Nie odtwarza brakujących
danych geometrii ani parametrów materiału w starszych zewnętrznych plikach.

## Binarne CXDLPv1

Otwieranie rozpoznaje zawartość .v1.cxdlp. Maski trafiają do tego samego
kanonicznego dokumentu PNG co pozostałe formaty; edycja nie zmienia
orientacji drukarki ponownie. Zapis koduje serie szarości do binarnego
kontenera, aktualizuje liczniki pikseli i zachowuje oryginalny nagłówek,
parametry ekspozycji, podglądy i stopkę. Rozszerzenie musi być .v1.cxdlp.
Importer kontroluje pełne tabele i odrzuca serie poza obrazem, nakładające
się piksele, niezgodne liczniki i ucięte dane. Odczyt modelu 3D z tego
formatu pozostaje oddzielnym, niezaimplementowanym zadaniem.

## Binarne CXDLP v2/v3

Importer rozpoznaje wersję na podstawie zawartości i przed dekodowaniem
sprawdza sumę XOR/CRC całego pliku. Zapis zachowuje ustawienia, wszystkie
miniatury i nagłówek informacyjny v3. Przelicza powierzchnię największego
konturu w mm²×1000 i obie tabele oraz sumę końcową. Model 3D nie jest
potrzebny do edycji maski; rekonstrukcja 3D pozostaje oddzielnym zadaniem.
Wersję v2 edytor zapisuje jako .v2.cxdlp, a v3 jako .cxdlp.

## Binarne CXDLPv4

Open layer archive rozpoznaje wersję po zawartości, weryfikuje CRC, tabele,
podglądy RGB15 i pełne pokrycie maski przez RLE. Obsługuje szyfrowanie
warstw oryginału i zachowuje je podczas zapisu. Save edited archive wymaga
rozszerzenia .cxdlpv4. Nie zmienia ustawień ekspozycji/ruchu ani miniatur.

Edycja importowanego v4 natychmiast kwantyzuje piksele do 7 bitów: 0/1
stają się 0, pozostałe wartości mają najmłodszy bit ustawiony. Undo/Redo
przywracają dokładnie takie obrazy, jakie trafią do pliku. Zapis aktualizuje
adresy po zmianie długości RLE, rozdziela edytowane współdzielone maski,
przelicza największe kontury oraz objętość z rzeczywistych Z. Masę i koszt
przelicza tylko z istniejących proporcji, bez zgadywania właściwości żywicy.
Zapis jest atomowy; błąd pozostawia dokument jako niezapisany.
Rekonstrukcja modelu 3D z tego kontenera pozostaje oddzielnym zadaniem.

## Box blur

Range operation → Box blur uśrednia szarość bez progowania. Radius (pixels)
określa nieparzysty kwadratowy filtr o boku 2×radius+1; 0 pozostawia obraz
bez zmian. Brzegi są odbijane bez powielania skrajnego piksela (Reflect101).
Wynik jest zaokrąglany dopiero po zsumowaniu obu osi, żeby nie tracić szarości
w pośrednim przebiegu. Maksymalny promień to 4096.

Zakres jest jednym krokiem Undo/Redo, a anulowanie w trakcie przetwarzania
pozostawia maski i historię nienaruszone. Operacja działa w tle przez istniejący
dialog postępu. Sumy są 64-bitowe; pamięć pomocnicza zależy od szerokości,
a koszt obliczeń jest liniowy względem liczby pikseli również na małych
obrazach z dużym promieniem. Dotyczy to wariantu BoxBlur z OperationBlur.

## Median blur

Range operation → Median blur zastępuje piksel medianą jego sąsiadów,
usuwając odosobnione jasne/ciemne piksele bez progowania całej maski.
Radius (pixels) określa kwadrat o boku 2×radius+1, od 0 do 4096.
Brzeg jest powielany (Replicate), zgodnie z medianBlur w UVtools/OpenCV.
Duży promień jest obsługiwany przez histogramy kolumn z licznikami 32-bitowymi;
pamięć pomocnicza zależy od szerokości obrazu, nie od powierzchni filtra.
Cały zakres tworzy jeden krok Undo/Redo. Anulowanie nie zmienia masek
ani historii, również gdy filtr zdążył przetworzyć część pierwszej warstwy.
Pozostałe warianty rozmycia (Stack, Pyramid, Gaussian, Filter 2D) są w Operations > Pixels > Blur (niżej).

## Narzędzia Text, Supports i Drain hole (Fala 42)

Trzeci rząd przycisków narzędzi (pod Freehand ... Flood fill) zawiera trzy narzędzia
działające na kliknięcie, jak Flood fill; każde użycie to jeden krok Undo.
Wiersz opcji pod „Grayscale / threshold” pokazuje tylko opcje wybranego narzędzia.

- **Text** (UVtools PixelText): pole tekstu (wiele wierszy), czcionka Hershey
  (Simplex, Plain, Duplex, Complex, Triplex, Complex small, Script simplex,
  Script complex) z opcją Italic, Font scale 0,2-200, Thickness (px), Angle
  (stopnie, dodatni zgodnie z ruchem wskazówek zegara), Alignment (Left / Center /
  Right) i Mirror (odbicie w pionie). Klik wpisuje tekst z lewym końcem linii
  bazowej w klikniętym punkcie jasnością z pola „Grayscale / threshold” (Eraser =
  czerń palety) na wszystkich warstwach zakresu zaznaczenia (jak pozostałe
  narzędzia). Pusty tekst jest odrzucany komunikatem; tylko znaki łacińskie.
- **Supports** (UVtools PixelSupport): Tip / Pillar / Base diameter (px). Klik rysuje
  kolumnę podpory od warstwy pod klikniętą w dół: koło o średnicy rosnącej od
  końcówki do słupka, a na najniższych warstwach średnicy podstawy, aż kolumna
  dotknie modelu. Kliknięta warstwa pozostaje bez zmian; zakres warstw nie ma
  wpływu. Klik na warstwie 1 jest odrzucany z wyjaśnieniem (nic pod nią).
- **Drain hole** (UVtools PixelDrainHole): Diameter (px). Klik wierci czarny otwór od
  warstwy pod klikniętą w dół, aż dotrze do pustej przestrzeni (koło >= 1/3
  czarne). Kliknięta warstwa pozostaje bez zmian.
Podgląd po wciśnięciu myszy pokazuje tekst albo koło pierwszej warstwy; Esc
anuluje, puszczenie zatwierdza.

## Nowe operacje w oknie Operations (Fala 42)

- **Pixels > Blur**: Stack, Box, Pyramid, Median, Gaussian i Filter 2D (jądro
  w tekście, wartości 0-255 bez normalizacji, kotwica X/Y). Rozmiar jądra musi
  być nieparzysty dla Stack, Median i Gaussian. Szybkie filtry Box / Median w
  combo „Range operation” zostają jako skrót (promień zamiast rozmiaru).
- **Pixels > Mask by image**: mnoży piksele przez szary obraz PNG (biel zachowuje,
  czerń czyści); „Resize to the layer size” skaluje obraz dwuliniowo do warstwy
  lub ROI, bez niego obraz o innym rozmiarze jest odrzucany.
- **Pixels > Redraw model**: przerysowuje podpory lub model z drugiego pliku
  „samego modelu” (ten sam obiekt bez podpór i tratwy), operator i jasność do
  wyboru.
- **Pixels > Raft relief**: nowy typ „Linked lines” (grubość linii, minimum
  połączeń, zewnętrzne podpory).
- **Layers > Layer import**: pliki PNG (po jednym na warstwę) i pliki drukarek,
  typy Insert, Replace, Stack, Merge (add / maximum), Subtract, Absolute difference,
  AND, OR, XOR; Insert, „Append layers” i „Discard layers after” zmieniają liczbę
  warstw i są dostępne tylko dla dokumentów wygenerowanych; dla archiwum
  otwartego z pliku i wyniku slicingu modelu działa wersja zmieniająca same
  piksele, a reszta jest odrzucana z powodem dokumentu.
- **Parameters > Stir resin**: warstwy mieszania żywicy na początku wydruku
  (Z = 0,001 mm); tylko dokumenty wygenerowane. Eksport takiego dokumentu jest
  możliwy tylko do formatów `uvj` i `cxdlpv4` (przez konwersję dokumentu);
  pozostałe formaty odmawiają przed zapisem z komunikatem o sekwencjach
  o powtórzonej wysokości.
Wszystko działa w tle z postępem i Cancel, w zakresie warstw i ROI okna, a każde
Apply to jeden krok Undo.
