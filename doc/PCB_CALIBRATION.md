# Natywne zadania PCB i kalibracyjne

Narzędzia są w menu **Window → PCB exposure** i **Window → Printer calibration**.
Korzystają z wybranej drukarki SLA oraz jej fizycznych wymiarów LCD. Nie tworzą
modelu pomocniczego ani nie uruchamiają UVtools/CLI. Zadanie zawiera dokument
warstw i rzeczywiste poziomy Z; po wygenerowaniu trafia do edytora pikseli.

## PCB

1. Wybierz drukarkę i sprawdź rozdzielczość, wymiary w mm, orientację i odbicia.
2. Otwórz PCB exposure. Wskaż pliki miedzi Gerber (Browse umożliwia wybór
   wielu plików) i opcjonalny wspólny plik wierceń Excellon. Pole Copper
   Gerber przyjmuje również jedną ścieżkę na wiersz. Sprawdź jednostki źródła.
   Zaznaczone Merge Gerber files into one exposure tworzy jedną maskę;
   po odznaczeniu powstaje osobna warstwa dla każdego pliku w kolejności listy.
3. Ustaw ekspozycję w sekundach, margines LCD, przesunięcie X/Y i odbicia
   samej płytki. Płytka jest centrowana bez skalowania.
4. Generate PCB layers tworzy maski. Sprawdź na nich ścieżki, otwory,
   odbicia i polaryzację. Invert exposure polarity odwraca całe pole LCD,
   włącznie z obszarem poza płytką.
5. W edytorze wybierz format i Export generated layers. Zamknięcie przed
   eksportem wymaga potwierdzenia odrzucenia zadania.

Zadanie PCB ma nominalne Z kolejnych warstw równe wielokrotności bieżącej
wysokości warstwy, bez warstw początkowych/przejściowych, z PWM 255, zerowymi wysokościami lift/retract
oraz zerowymi dodatkowymi oczekiwaniami. Używa wskazanej ekspozycji i gamma 1.
Dłuższa ekspozycja rozszerza granice czasu w kopii konfiguracji zadania;
nie zmienia zapisanego profilu drukarki/materiału ani sceny. Zachowanie fizycznej
drukarki zależy od jej firmware i interpretacji wybranego kontenera.

Wiele plików zachowuje wspólne współrzędne w mm. Centrowanie i odbicia
korzystają z obrysu wszystkich plików, więc nie przesuwają warstw osobno.
Scalanie tworzy sumę miedzi; wspólne wiercenia są odejmowane od każdego pliku.
Źródła powinny używać tego samego początku układu współrzędnych. Nie ma
jeszcze GUI do wyrównywania plików o różnych początkach ani polaryzacji
ustawianej osobno dla każdego źródła. Puste źródło zgłasza błąd.

Importer obsługuje:

- Gerber RS-274X: jednostki mm/cale, absolutne współrzędne FS L/T,
  apertury C/R/O/P, okrągłe otwory apertur, D01/D02/D03 i modalne współrzędne;
- interpolację liniową oraz wielokwadrantowe łuki G02/G03 po G75;
- regiony G36/G37 z konturami, polaryzację LPD/LPC i atrybuty X2
  niezmieniające geometrii;
- makra AM: koło, linia wektorowa, prostokąt, zamknięty kontur i wielokąt
  foremny, moiré (6) i pola termiczne (7); parametry $n, przypisania,
  nawiasy i arytmetykę +/−/x/÷;
- Excellon: jednostki, format współrzędnych, LZ/TZ, deklaracje narzędzi,
  absolutne trafienia, współrzędne dziesiętne, szczeliny G85 i odejmowanie
  otworów. Początek G85 może być podany przed poleceniem lub zachowany
  z poprzedniej pozycji; przekrój ma średnicę wybranego narzędzia.

Makro najpierw tworzy lokalną geometrię. Jego przezroczysty otwór nie usuwa
miedzi wcześniej naniesionej inną aperturą. Kolejność globalnych operacji
ciemnych/jasnych pozostaje zachowana. Krzywe są przybliżane z tolerancją
ćwierci mniejszego fizycznego wymiaru piksela. Obrót i odbicia LCD nakłada wspólny
rasteryzator slicera; geometria wejściowa pozostaje w mm.

Nadal brak m.in. step-and-repeat, aperture blocks, dodatkowych transformacji
Gerbera, single-quadrant G74, kreślenia aperturą makro/z otworem
i ciągłego frezowania Excellon. Te polecenia zgłaszają błąd. Nie są
ignorowane przy tworzeniu maski. Nie jest to jeszcze kompletny port PCB UVtools.

Wejście ma limit 64 MiB i miliona poleceń. Rozdzielczość maski ma limit
32768 na oś i 128 Mi pikseli. Nadmierna geometria jest odrzucana. Płytka,
która nie mieści się z marginesem, jest odrzucana zamiast automatycznego
zmniejszania lub obcinania.

## Pierwsze wzorce kalibracyjne

- **XYZ blocks**: dziewięć pełnych prostokątnych bloków w siatce 3×3;
- **Grayscale patches**: pola z równomiernie rosnącymi wartościami szarości,
  w kolejności wierszami od dolnego lewego rogu fizycznego LCD;
- **Lift area**: pojedynczy centrowany prostokąt o zadanych rozmiarach.

Rozmiary są w mm. Wysokość testu musi być wielokrotnością wysokości warstwy.
Ekspozycje i ruch pochodzą z bieżących presetów; transition jest wyłączone.
Pierwsza warstwa ma tę samą wysokość co następne. Pola szarości mają gamma 1;
kontenery redukujące głębię szarości mogą zmienić poziomy po eksporcie.

Te wzorce nie zastępują wszystkich OperationCalibrate UVtools. Nadal brak
m.in. podpisów, reliefów, wariantów pustych modeli, prób tolerancji, elephant
foot, blooming, stress tower i exposure finder. Edytor umożliwia zmianę masek,
jednak nie stanowi edytora parametrów ekspozycji pojedynczego poziomu.

## Testy

`ctest --test-dir build -R sla_pcb_calibration --output-on-failure`
sprawdza fizyczną geometrię apertur/regionów/łuków/makr, kolejność polaryzacji,
jednostki, konwencje zer Excellon, atomowość przy błędzie/anulowaniu, limity,
osiem kombinacji orientacji/odbicia LCD i wartości wzorców kalibracyjnych.

Lokalny skrypt QA GUI (poza repozytorium) używa odizolowanych profili i lokalnych
plików Gerber/Excellon. Wyniki GUI trzeba oceniać osobno od testów jednostkowych.

Makra moiré i thermal mają lokalnie wycinane otwory/przerwy, więc nie
usuwają geometrii wcześniej narysowanej w makrze. Obrót obejmuje także
środek i krzyże. Liczba pierścieni oraz liczba wierzchołków mają limity;
geometria wierceń i szczelin też ma budżet miliona wierzchołków.

Weryfikacja wielu plików (2026-10-04): pcb-multiple-tests.log — 198 asercji
w 15 przypadkach PCB/kalibracji, obie orientacje, odbicia, wspólne położenie,
brak nakładania i zgodność sumy masek, anulowanie oraz odrzucanie złych wejść.
gui-pcb-multiple/result.json — eksport scalony i osobny przez GUI, wspólne
wiercenia, identyczna suma wszystkich pikseli, nominalne Z i stacjonarna
ekspozycja 30 s; brak prób połączeń. pcb-multiple-ctest.log — 7/7.
