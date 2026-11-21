# Lista zmian

## 1.0.0 — 2026-11-21

Pierwsze wydanie SLA-Slicer: PrusaSlicer 2.9.4 ograniczony do SLA, działający offline.

- Slicing SLA (podpory, w tym organiczne eksperymentalne, wydrążanie, pad, wyspy)
  bez G-code, FFF/FDM i funkcji sieciowych.
- Zapis w 80 identyfikatorach formatów drukarek; odczyt i edycja wielu z nich.
- Edytor warstw (widok Sliced): pędzel, gumka, zakresy, operacje UVtools na maskach,
  wykrywanie problemów, Undo/Redo, narzędzia płótna, tekst i import obrazu.
- Ekspozycja PCB z plików Gerber i Excellon.
- Kalibracje odpowiadające UVtools (Exposure Finder, tabele ekspozycji, wzorce).
- Statystyki żywicy i zużycia, katalog żywic (`resin_stock.json`).
- CLI i automatyzacja (`--help-sla`, wyniki JSON, kody wyjścia).
- Własny katalog danych `~/.config/SLA-Slicer` z jednorazowym kopiowaniem części SLA
  z `~/.config/PrusaSlicer` przy pierwszym uruchomieniu.
- Interfejs po angielsku i po polsku (inne języki systemu: angielski); strona podręcznika
  `man sla-slicer`, metadane AppStream, archiwum źródeł oraz sumy kontrolne/podpisy wydania.
- Pakiety Linux x86_64: AppImage, .deb, .rpm i przenośne archiwum tar.gz.
- Budowanie: `build_linux.sh` (z kontrolą wymagań, `--bundled-opencv`, `--docker`),
  `build_win.bat` (eksperymentalny); OpenCV 4.12.0 (core, imgproc) może być budowany
  razem z zależnościami.

### Znane ograniczenia

- Brak potwierdzenia na prawdziwych drukarkach: żaden format nie został wydrukowany;
  zgodność sprawdzano niezależnym odczytem (UVtools i niezależne dekodery).
- Narzędzie tekstu: tylko podstawowa łacina.
- Import obrazu: tylko PNG.
- Eksport „stir resin”: tylko uvj i cxdlpv4.
- Operacja „I printed this file” nie ma cofania (Undo).
- Podpory organiczne są eksperymentalne.
- Skala tekstu 1.5 powoduje przewijanie na niektórych stronach.
- Przy GDK_SCALE=2 i rozdzielczości 1280x800 jeden przycisk jest przycięty.
- Poza zakresem: skrypty C# (UVtools), SL1SVG z rastra, edycja SVGX, JXS bez action_list.
- Windows: build niesprawdzony (`build_win.bat` eksperymentalny).
- Pakiety tylko dla Linuksa x86_64.
