# Instalacja i budowanie SLA-Slicer

Wersja produktu: 1.0.0. Aplikacja jest tylko SLA i działa w całości offline.
Instrukcja obsługi: [manual.txt](manual.txt). Kod i architektura: [doc/DEVELOPER.md](doc/DEVELOPER.md).

## 1. Gotowe pakiety (Linux x86_64)

Wymagania systemu: glibc 2.35 lub nowszy, libstdc++ z `GLIBCXX_3.4.30` (GCC 12+), GTK3,
OpenGL (Mesa lub sterownik producenta), dbus, libSM/libICE, libnotify oraz zainstalowana
lokalizacja UTF-8 (bez niej program zgłasza „Switching language failed” — dotyczy minimalnych
obrazów kontenerów). Interfejs jest dostępny po angielsku i po polsku; dla każdego innego języka systemu
(np. `LANG=de_DE.UTF-8`) program uruchamia się po angielsku, także gdy taka lokalizacja nie jest
wygenerowana w systemie.

```sh
# AppImage (bez FUSE dodaj --appimage-extract-and-run)
chmod +x SLA-Slicer-1.0.0-x86_64.AppImage && ./SLA-Slicer-1.0.0-x86_64.AppImage

# Debian, Ubuntu
sudo apt install ./sla-slicer_1.0.0_amd64.deb
sudo apt remove sla-slicer

# Fedora
sudo dnf install ./sla-slicer-1.0.0-1.x86_64.rpm
sudo dnf remove sla-slicer

# przenośne archiwum (bez instalacji)
tar xzf sla-slicer-1.0.0-linux-x86_64.tar.gz && ./sla-slicer-1.0.0/sla-slicer
```

Pakiety .deb/.rpm instalują wydanie do `/opt/sla-slicer`, polecenie `sla-slicer` do `/usr/bin`,
wpis pulpitu, ikony i metadane AppStream; licencje i SBOM trafiają do
`/usr/share/doc/sla-slicer`. Archiwum i pakiety zawierają te same pliki. Pakiety instalują też
stronę podręcznika `man sla-slicer` oraz plik AppStream
`/usr/share/metainfo/io.github.pieotr.SLASlicer.metainfo.xml` (wpis pulpitu to
`io.github.pieotr.SLASlicer.desktop`).

### 1.1. Weryfikacja pobranych plików

Każde wydanie ma plik `SHA256SUMS` z sumami kontrolnymi wszystkich plików oraz (jeśli wydanie
jest podpisane) podpis `SHA256SUMS.asc`. Pobierz pliki do jednego katalogu i sprawdź:

```sh
sha256sum -c SHA256SUMS                      # sumy kontrolne (--ignore-missing, gdy pobrano tylko część plików)
gpg --verify SHA256SUMS.asc SHA256SUMS       # podpis; wymaga klucza wydawcy: gpg --import <klucz.asc>
```

`sha256sum` ma wypisać `OK` przy każdym pliku, a `gpg` — `Good signature` dla klucza wydawcy
(sprawdź jego odcisk palca poza tym wydaniem, np. na stronie projektu). Brak pliku `SHA256SUMS.asc`
oznacza wydanie niepodpisane: sumy kontrolne chronią wtedy tylko przed uszkodzeniem pliku.

## 2. Budowanie ze źródeł na Linuksie

### 2.1. Wymagania

Potrzebne: kompilator C++17 (GCC 12 lub nowszy), CMake >= 3.16 (do budowy zależności zalecany
>= 3.28), git, make lub ninja, pkg-config, autoconf/automake/libtool/m4 i texinfo (budowa GMP/MPFR),
patch, nagłówki GTK3, OpenGL/GLU, dbus, TIFF, PNG, JPEG, OpenJPEG, zlib, X11 oraz **OpenCV >= 4.7**
(moduły core i imgproc; potrzebne `cv::stackBlur`). Zależności pozostałe (Boost, wxWidgets, TBB,
CGAL, OpenVDB, ...) skrypt buduje sam ze źródeł i linkuje statycznie. Potrzeba ok. 8 GB RAM; przy
mniejszej pamięci użyj mniejszego `--jobs`. Ścieżka do repozytorium nie może zawierać spacji.
Pierwsze budowanie zależności trwa długo (godziny); wynik jest w `deps/build`.

`bash ./build_linux.sh --check` sprawdza brakujące narzędzia i biblioteki oraz wypisuje polecenie
instalacji dla wykrytej dystrybucji.

**Debian / Ubuntu** (lista sprawdzona na Ubuntu 22.04 przez `tools/docker/Dockerfile.build`):

```sh
sudo apt-get install -y build-essential git cmake ninja-build pkg-config autoconf automake libtool m4 \
  texinfo patch curl unzip xz-utils \
  libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev libdbus-1-dev libsm-dev libice-dev \
  libnotify-dev libxkbcommon-dev libwayland-dev libx11-dev libxext-dev libxrandr-dev libxi-dev \
  libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev zlib1g-dev libmspack-dev
```

OpenCV: pakiet `libopencv-dev` ma wymaganą wersję dopiero od Debiana 13 / Ubuntu 25.04 (4.10);
Debian 12, Ubuntu 22.04 i 24.04 mają 4.5/4.6 — użyj `--bundled-opencv` (p. 2.3). Na Ubuntu 22.04
CMake z apt (3.22) jest za stary dla budowy zależności: `pip3 install 'cmake>=3.28,<4'`.

**Fedora** (nazwy pakietów wg znajomości dystrybucji, nie sprawdzone w tym repozytorium):

```sh
sudo dnf install -y gcc gcc-c++ make cmake ninja-build git pkgconf-pkg-config autoconf automake libtool m4 \
  texinfo patch gtk3-devel mesa-libGL-devel mesa-libGLU-devel libEGL-devel dbus-devel libSM-devel \
  libICE-devel libnotify-devel libxkbcommon-devel libX11-devel libtiff-devel libpng-devel \
  libjpeg-turbo-devel openjpeg2-devel zlib-devel opencv-devel
```

**Arch Linux / CachyOS** (nazwy pakietów wg znajomości dystrybucji; budowanie sprawdzone na CachyOS
z systemowym OpenCV 5.1):

```sh
sudo pacman -S --needed base-devel git cmake ninja pkgconf autoconf automake libtool m4 texinfo patch \
  gtk3 libglvnd glu dbus libsm libice libnotify libxkbcommon libx11 libtiff libpng libjpeg-turbo \
  openjpeg2 zlib opencv
```

### 2.2. Budowanie

```sh
bash ./build_linux.sh --run            # sprawdzenie wymagań, zależności (deps/build), aplikacja (build/), test --help
bash ./run_sla_slicer.sh               # uruchomienie
bash ./run_sla_slicer.sh --datadir /sciezka/do/proby    # z osobnym katalogiem danych
bash ./build_linux.sh --help
```

Opcje: `--jobs N` (równoległość), `--clean` (usuwa `build` i `deps/build`), `--deps` (wymusza
przebudowę zależności), `--tests` (buduje testy SLA i uruchamia szybki podzbiór CTest),
`--bundled-opencv`, `--check`, `--skip-checks`, `--docker`. Plik wykonywalny: `build/src/sla-slicer`.
Nie wolno przenosić katalogu `deps/build/destdir` po zbudowaniu (wxWidgets zapisuje ścieżkę).

Ręcznie (to samo, co robi skrypt):

```sh
cmake -S deps -B deps/build -DDEP_WX_GTK3=ON && cmake --build deps/build -- -j1
cmake -S . -B build -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DSLIC3R_PCH=OFF \
      -DCMAKE_PREFIX_PATH=$PWD/deps/build/destdir/usr/local
cmake --build build --target PrusaSlicer -j8
```

### 2.3. OpenCV

Domyślnie na Linuksie używany jest OpenCV z systemu (`find_package(OpenCV)`, wymagane >= 4.7;
starszy kończy konfigurację błędem). Jeśli system nie ma OpenCV >= 4.7, zbuduj go razem z
zależnościami:

```sh
bash ./build_linux.sh --bundled-opencv        # = cmake -S deps ... -DDEP_OpenCV=ON; wersja 4.12.0, core+imgproc, statycznie
```

Zależność jest zdefiniowana w `deps/+OpenCV/OpenCV.cmake` (OpenCV 4.12.0, ten sam hash co w
`tools/docker/Dockerfile.build`). Gdy w prefiksie zależności (`deps/build/destdir/usr/local`)
jest OpenCV, CMake wybiera właśnie jego. Na Windows i macOS zależność jest budowana domyślnie.
Wybrać inną instalację można przez `-DOpenCV_DIR=<katalog z OpenCVConfig.cmake>`.

### 2.4. Instalacja z drzewa budowania

```sh
cmake --install build --prefix /opt/sla-slicer      # bin/sla-slicer + resources/
```

Plik wykonywalny szuka zasobów w `<katalog exe>/../resources`, więc drzewo można przenosić.
`-DSLIC3R_FHS=1` zapisuje na stałe ścieżkę `<prefix>/share/SLA-Slicer` (nieprzenośne).

## 3. Przenośne archiwum i pakiety natywne

### 3.1. Przenośne archiwum w kontenerze Docker (glibc 2.35)

Wymaga działającego dockera i sieci w czasie budowania (apt, PyPI, źródła zależności); wynik działa offline.

```sh
bash ./build_linux.sh --docker                       # lub: JOBS=8 bash tools/docker/build_portable.sh
# -> build-docker/dist/sla-slicer-<wersja>/ i sla-slicer-<wersja>-linux-x86_64.tar.gz
```

Do kontenera trafia dokładnie zestaw plików repozytorium (`git ls-files -co --exclude-standard`); bez
`.git` (archiwum źródeł) kopiowane jest całe drzewo bez katalogów budowania. `tools/docker/Dockerfile.build`
definiuje obraz (Ubuntu 22.04, GCC 12, OpenCV 4.12.0 w `/opt/opencv`), `tools/docker/container_build.sh`
buduje zależności (pierwszy raz godziny; cache w `build-docker/work`, ccache w `build-docker/ccache`),
aplikację, uruchamia testy CTest niewymagające ekranu i pakuje wydanie.
`BUNDLE_LIBSTDCXX=1` dołącza libstdc++/libgcc_s.

### 3.2. Archiwum z lokalnej kompilacji

```sh
bash tools/make_relocatable_bundle.sh        # dist/sla-slicer-<wersja>-linux-x86_64.tar.gz (z gotowego build/)
```

Skrypt używa `tools/bundle_native_libraries.py` i usuwa wszystkie biblioteki, które musi dostarczyć
system (glibc, libstdc++, GTK/GL/X11/Wayland i ich zależności); zostają biblioteki specyficzne dla
aplikacji. Takie archiwum działa tylko na dystrybucjach z glibc co najmniej tak nowym jak na maszynie
budującej — do szerokiej dystrybucji użyj wersji z p. 3.1.

### 3.3. AppImage, .deb, .rpm

Powstają z przenośnego archiwum (wymaga dockera: obrazy `ubuntu:22.04` dla `dpkg-deb`/`lintian`
i `fedora:latest` dla `rpmbuild`; appimagetool 1.9.1 i środowisko uruchomieniowe type2 są przypięte
przez SHA256 w skrypcie):

```sh
bash tools/release/make_native_packages.sh dist/sla-slicer-1.0.0-linux-x86_64.tar.gz /sciezka/wyjscia
# -> SLA-Slicer-1.0.0-x86_64.AppImage, sla-slicer_1.0.0_amd64.deb, sla-slicer-1.0.0-1.x86_64.rpm, SHA256SUMS, lintian.txt
```

Wpis pulpitu w pakietach nazywa się `io.github.pieotr.SLASlicer.desktop` (identyfikator komponentu
AppStream z `src/platform/unix/io.github.pieotr.SLASlicer.metainfo.xml`); skrypt odmawia pracy, gdy
plik AppStream nie ma wpisu `<release version="<wersja>">`. Strona podręcznika to
`src/platform/unix/sla-slicer.1` (w pakietach: `/usr/share/man/man1/sla-slicer.1.gz`).

### 3.4. Archiwum źródeł, podpisy i całe wydanie

```sh
bash tools/release/make_source_tarball.sh /sciezka/wyjscia   # sla-slicer-<wersja>-src.tar.gz
bash tools/release/sign_release.sh /sciezka/wyjscia [KLUCZ_GPG]   # SHA256SUMS (+ SHA256SUMS.asc z kluczem)
bash tools/release/make_release.sh dist/sla-slicer-1.0.0-linux-x86_64.tar.gz dist/sla-slicer-1.0.0
```

Archiwum źródeł zawiera dokładnie zestaw plików repozytorium (`git ls-files -co --exclude-standard`),
jest odtwarzalne (ta sama zawartość daje ten sam SHA256; czas z `SOURCE_DATE_EPOCH` lub z ostatniego
commita) i zawiera plik `BUILD_ID`, dzięki czemu budowa bez `.git` pokazuje ten sam identyfikator
budowy co budowa z repozytorium. `sign_release.sh` bierze klucz z drugiego argumentu lub ze zmiennej
`SLA_SLICER_GPG_KEY`; bez klucza zapisuje tylko `SHA256SUMS` i wypisuje, jak podpisać później
(`--per-file` dodaje podpisy `.asc` każdego pliku). `make_release.sh` składa katalog wydania:
archiwum przenośne, AppImage, .deb, .rpm, archiwum źródeł, `RELEASE_NOTES.md`, `SHA256SUMS`
(i `SHA256SUMS.asc`, jeśli podano klucz).

## 4. Windows (EKSPERYMENTALNE, niesprawdzone)

`build_win.bat` jest dostosowaną wersją skryptu PrusaSlicera i **nigdy nie był uruchomiony** (autor
nie miał dostępu do Windows ani MSVC). Wymaga Visual Studio 2019/2022 z narzędziami C++ (CMake w
zestawie), gita i dostępu do sieci. Zależności (w tym OpenCV 4.12.0, statycznie, tylko Release)
buduje skrypt; konfiguracje: `RelWithDebInfo` (domyślna), `Release`, `MinSizeRel` — `Debug` nie jest
obsługiwany. Uruchom w „x64 Native Tools Command Prompt”:

```bat
build_win -d "c:\src\SLA-Slicer-deps"       :: pierwsze budowanie (zależności + aplikacja)
build_win -r console                        :: kolejne budowania aplikacji i uruchomienie sla-slicer-console.exe
build_win -?                                :: pomoc
```

Wynik: `build\src\<konfiguracja>\sla-slicer.exe` i `sla-slicer-console.exe`. Znane ryzyka: skrypt i
CMake dla Windows mogą wymagać poprawek; nazwy plików ze znakami spoza ASCII w niektórych zapisach
formatów nie były sprawdzane. Zgłoś problemy, jeśli uda się zbudować.

## 5. Nazwy, wersje i dane użytkownika

- Nazwa dla użytkownika: **SLA-Slicer** (`SLIC3R_APP_NAME` w `version.inc`); klucz `SLIC3R_APP_KEY`
  (`SLA-Slicer`) nazywa katalog danych (`~/.config/SLA-Slicer` lub `$XDG_CONFIG_HOME/SLA-Slicer`),
  plik `SLA-Slicer.ini`, nazwę aplikacji wx, domenę gettext i nazwy instancji D-Bus — zainstalowany
  PrusaSlicer nigdy nie dzieli konfiguracji z SLA-Slicerem.
- Pierwsze uruchomienie: jeśli nowy katalog nie istnieje, nie podano `--datadir`, a istnieje
  `~/.config/PrusaSlicer`, jego część SLA (profile procesów/żywic/drukarek, paczki producentów SLA,
  `shapes/`, `resin_stock.json`, przefiltrowany `SLA-Slicer.ini`) jest KOPIOWANA; katalog PrusaSlicera
  nie jest zmieniany. `SLA_SLICER_NO_MIGRATION=1` wyłącza kopiowanie. Logika:
  `src/libslic3r/LegacyDataMigration.{hpp,cpp}`.
- Wersje: `SLA_SLICER_VERSION` (1.0.0) to wersja produktu (tytuł okna, About, `--version`, nazwy
  pakietów). `SLIC3R_VERSION` (2.9.4) to wersja bazy kodu PrusaSlicer używana do zgodności
  (paczki producentów, nagłówki ini/3MF) i nie jest podbijana przy wydaniu.
- Identyfikator budowy: `SLIC3R_BUILD_ID` = `SLA-Slicer-<wersja>+<sufiks>`; sufiks to
  `SLA_SLICER_BUILD_ID` (zmienna CMake lub środowiska), albo pierwsza linia pliku `BUILD_ID` w
  katalogu głównym (zapisywanego do archiwum źródeł), albo `git describe --always --dirty`, albo
  `source` bez gita. Budowa w dockerze liczy go na hoście.
- Plik wykonywalny to `sla-slicer` (drzewo budowania: `build/src/sla-slicer`).
- Aplikacja jest tylko offline: nie linkuje bibliotek sieciowych (webkit, soup, curl, ssl), CURL i
  OpenSSL nie są budowane. Źródła zależności są pobierane tylko podczas budowania.

## 6. Naprawa po aktualizacji kompilatora

Biblioteki statyczne zawierające bytecode LTO muszą pasować do kompilatora linkującego. Komunikat
`libtbb.a ... generated with LTO version ... instead of the expected ...` oznacza, że zależność
trzeba zbudować ponownie (`bash ./build_linux.sh --deps`, w ostateczności `--clean --deps`).
