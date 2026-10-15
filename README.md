<div align="center">

# FS Editor

**Wieloplatformowy edytor napisów ASS/SSA: logika i API z Aegisuba, układ z Kainote.**

[![Licencja: BSD-3-Clause](https://img.shields.io/badge/licencja-BSD--3--Clause-blue)](LICENSE)
[![Rust 1.99](https://img.shields.io/badge/Rust-1.99-orange?logo=rust)](rust-toolchain.toml)
[![Qt 6.8+](https://img.shields.io/badge/Qt-6.8%2B-41cd52?logo=qt)](app/CMakeLists.txt)
![Platformy](https://img.shields.io/badge/platformy-Windows%20%7C%20Linux-lightgrey)

[Funkcje](#funkcje) • [Budowanie](#budowanie) • [Użycie](#użycie) • [Testy](#testy) • [Struktura projektu](#struktura-projektu) • [Dokumentacja](#dokumentacja)

</div>

FS Editor to edytor napisów do typesettingu, timingu i tłumaczenia. Rdzeń jest napisany w Rust i nie zna GUI: te same operacje wywołuje interfejs Qt, narzędzie wiersza poleceń `fsed` i skrypty Lua. Plik otwarty i zapisany bez zmian jest identyczny co do bajtu.

| Warstwa | Technologia |
|---|---|
| Rdzeń | Rust (edycja 2024): model dokumentu, bezstratny parser ASS, undo, VFR |
| Interfejs | Qt 6 Widgets (C++23), wideo i audio rysowane na GPU przez QRhi |
| Most | [`cxx`](https://cxx.rs) + [Corrosion](https://github.com/corrosion-rs/corrosion) |
| Media | [BestSource](https://github.com/vapoursynth/bestsource), [libass](https://github.com/libass/libass), cpal, realfft |
| Automatyzacja | LuaJIT przez `mlua`, zgodność z Aegisub Automation 4 |

> [!NOTE]
> Projekt jest we wczesnej wersji (0.1.0) i nie ma jeszcze wydań. Paczki ZIP dla Windows i Linuksa powstają w CI jako artefakty. Kod przewiduje macOS, ale nie był jeszcze budowany ani testowany na Macu.

## Funkcje

- **Bezstratny ASS.** BOM, końce linii, komentarze, nieznane sekcje, `[Fonts]`, `[Graphics]` i dane projektu Aegisuba zostają nietknięte. Linia, której nikt nie zmienił, jest zapisywana z oryginalnego tekstu.
- **Formaty:** ASS, SSA, SRT, MPL2, MicroDVD i TMP, z wykrywaniem kodowania (UTF-8, UTF-16, Windows-1250 i inne).
- **Wideo co do klatki:** BestSource, VFR (timecody v1 i v2), keyframe'y, rozdziały, podgląd libass, zoom, tryb odtwarzacza i wideo zastępcze.
- **Audio:** waveform i spektrogram na GPU, tryb karaoke, odtwarzanie i zapis wycinka do WAV.
- **Narzędzia wizualne:** `\pos`, `\move`, `\org`, `\clip`/`\iclip`, rysunki `\p`, skalowanie i obroty bezpośrednio na wideo.
- **Timing:** przesuwanie czasów myszą w gridzie i panel zmiany czasów jak w Kainote, postprocesor (lead-in/out, łączenie sąsiednich linii, przyciąganie do keyframe'ów), kanji timer, zmiana FPS i resample rozdzielczości.
- **Tryb tłumaczenia** zgodny z Kainote: dwa pola tekstu, kolumna oryginału w gridzie i dopasowywanie tłumaczenia do oryginału.
- **Automatyzacja:** skrypty Aegisub Automation 4 w Lua i MoonScript, w tym karaskel, kara-templater i DependencyControl.
- **Narzędzia do pracy:** menedżer stylów, kolekcjoner czcionek (także z załączników MKV), załączniki, sprawdzanie pisowni (Hunspell), szukaj i zamień, zaznaczanie linii, porównanie napisów z dwóch kart, historia zmian i autozapis.
- **Karty:** każdy plik ma własny dokument, wideo, audio i historię undo.
- **Interfejs po polsku i angielsku,** język idzie za systemem albo za ustawieniem.

## Budowanie

### Wymagania

| Narzędzie | Potrzebne do |
|---|---|
| Rust 1.99 (`rustup` pobierze go sam z [rust-toolchain.toml](rust-toolchain.toml)) | wszystkiego |
| Kompilator C/C++: MSVC, GCC albo Clang | LuaJIT, mediów i GUI |
| [vcpkg](https://vcpkg.io) (`VCPKG_ROOT` albo vcpkg z Visual Studio) | wideo i audio |
| Qt 6.8+ z modułem Qt Shader Tools, CMake 3.28+ | GUI |

Na Linuksie potrzebne są też pakiety systemowe dla vcpkg i ALSA (Ubuntu):

```bash
sudo apt-get install -y libasound2-dev ninja-build nasm libgl1-mesa-dev libxkbcommon-dev autoconf-archive bison flex gperf zip
```

> [!IMPORTANT]
> Na Windows zmienna `NoDefaultCurrentDirectoryInExePath` przerywa budowanie LuaJIT, bo `msvcbuild.bat` uruchamia `minilua` z bieżącego katalogu. Jeśli jest ustawiona, usuń ją na czas budowania, np. `env -u NoDefaultCurrentDirectoryInExePath cargo build`.

### 1. Rdzeń i CLI

Do samego rdzenia i `fsed` wystarczy Rust i kompilator C:

```bash
git clone https://github.com/Ognisty321/fs-editor.git
```

```bash
cd fs-editor && cargo build --release -p fs-cli
```

Binarka trafia do `target/release/fsed`.

### 2. Biblioteki mediów

Wideo i audio (FFmpeg, libass, BestSource) instaluje skrypt z [vcpkg.json](vcpkg.json). Zapisuje ich położenie w `.cargo/config.toml`, więc cargo znajdzie je samo; CMake dostaje je przez `FS_DEPS_DIR`.

```bash
pwsh tools/deps.ps1
```

```bash
TRIPLET=x64-linux-dynamic tools/deps.sh
```

Pierwsze polecenie jest dla Windows, drugie dla Linuksa i macOS (wymaga `VCPKG_ROOT`).

> [!NOTE]
> Biblioteki lądują poza repozytorium (`%LOCALAPPDATA%\fs-editor\deps` albo `~/.cache/fs-editor/deps`), bo port FFmpeg w vcpkg nie znosi spacji w ścieżce.

Z bibliotekami można zbudować CLI z poleceniami `fsed media`:

```bash
cargo build --release -p fs-cli --features media
```

### 3. GUI

GUI buduje CMake, a rdzeń w Rust dobudowuje przez Corrosion. Ścieżki do Qt i bibliotek z kroku 2 podaje się przy konfiguracji.

**Windows** (Visual Studio 2026):

```bash
cmake -S app --preset windows -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DFS_DEPS_DIR="$LOCALAPPDATA/fs-editor/deps/installed/x64-windows"
```

```bash
cmake --build app/build-windows --config Release
```

**Linux** (sprawdzone na Ubuntu 26.04 z GCC i Qt 6.12 z aqtinstall) i macOS:

```bash
cmake -S app --preset unix -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64 -DFS_DEPS_DIR=$HOME/.cache/fs-editor/deps/installed/x64-linux-dynamic
```

```bash
cmake --build app/build-unix
```

> [!TIP]
> Preset `windows-dev` buduje rdzeń profilem cargo `gui-dev` (bez LTO, przyrostowo), co bardzo skraca kolejne kompilacje podczas pracy nad GUI.

### 4. Paczka ZIP

CPack składa paczkę z programem, Qt, bibliotekami mediów i dołączonymi skryptami automatyzacji:

```bash
cd app/build-windows && cpack -C Release
```

Na Linuksie paczka ma RUNPATH `$ORIGIN/../lib`. Z systemu potrzebne są tylko biblioteki X11/xcb, których wymaga Qt (`libxcb-cursor0`, `libxkbcommon-x11-0` i pokrewne), obecne na zwykłym pulpicie.

## Użycie

### Edytor

```bash
fsed napisy.ass wideo.mkv
```

Pliki podane w wierszu poleceń otwierają się w karcie. Napisy, wideo, audio i keyframe'y można też przeciągnąć w dowolne miejsce okna. Wideo z tekstowymi ścieżkami napisów (ASS, SSA, SRT) otwarte w pustej karcie pyta, czy wczytać którąś z nich.

**Sprawdzanie pisowni** używa słowników Hunspell (`xx_YY.aff` + `xx_YY.dic`). Wrzuć je do katalogu `dictionaries/` obok programu albo wskaż folder i język w ustawieniach. Aktualne słowniki: polski z [sjp.pl](https://sjp.pl/sl/en/), angielski z [en-wl/wordlist](https://github.com/en-wl/wordlist/releases).

### Wiersz poleceń

`fsed` z crate'u `fs-cli` wykonuje operacje rdzenia bez GUI. Kilka przykładów:

```bash
# opóźnienie napisów o 1,2 s od 10. minuty
fsed shift odc01.ass --by 1.2s --after 0:10:00.00 --in-place

# konwersja do SRT w CP1250
fsed convert odc01.ass -o odc01.srt --out-encoding windows-1250 --no-bom

# linie powyżej 20 znaków na sekundę, bez tagów
fsed lines odc01.ass --dialogue --min-cps 20 --strip

# kara-templater na całym pliku
fsed lua crates/fs-automation/automation/autoload/kara-templater.lua kara.ass --macro "Apply karaoke template" -o kara-fx.ass
```

Pełna lista poleceń (`info`, `check`, `fonts`, `resample`, `postprocess`, `tl`, `media` i inne) jest w [docs/CLI.md](docs/CLI.md).

## Testy

```bash
cargo test
```

Domyślne `cargo test` obejmuje crate'y, które nie potrzebują bibliotek mediów. Po kroku 2 z [budowania](#2-biblioteki-mediów) dochodzą testy mediów i mostu:

```bash
cargo test -p fs-media -p fs-bridge
```

Testy korpusowe sprawdzają każdy plik pod kątem zapisu bajt w bajt, nienaruszania sąsiednich pól przy edycji, bezstratnego parsowania tagów oraz konwersji przez SRT, MPL2, MicroDVD, TMP i SSA. Pliki biorą z:

- [`tests/corpus/`](tests/corpus/README.md): pliki testowe Aegisuba, timecody VFR, keyframe'y i próbki kodowań;
- `tests/corpus-local/`: twoje własne pliki `.ass` (katalog jest w `.gitignore`);
- katalogu ze zmiennej `FS_CORPUS_DIR`.

<details>
<summary>Testy opcjonalne i media testowe</summary>

| Zmienna | Co włącza |
|---|---|
| `FS_DEPCTRL_DIR=/ścieżka/do/DependencyControl` | test zgodności z DependencyControl (`cargo test -p fs-automation dependency_control`) |
| `FS_DICT=/ścieżka/pl_PL`, `FS_DICT_EN=/ścieżka/en_US` | testy pisowni na prawdziwych słownikach (`cargo test -p fs-bridge spell`), ścieżki bez rozszerzenia |

Media w `tests/media` generuje `tools/gen-test-media.sh` (potrzebne ffmpeg z libx264 i mkvmerge). Każda klatka jest jednolicie szara, a jej jasność koduje numer klatki, więc testy sprawdzają dokładność seeków po jednym pikselu.

</details>

<details>
<summary>Smoke test GUI</summary>

GUI czyta zmienne środowiskowe, które pozwalają sprawdzić aplikację bez klikania. Smoke test używa osobnych ustawień („FS Editor smoke test”), więc nie rusza twojej konfiguracji.

| Zmienna | Działanie |
|---|---|
| `FSED_SMOKE=plik.png` | zapisuje zrzut okna (albo otwartego dialogu) i zamyka program |
| `FSED_SMOKE_MS` | czas do zrzutu w ms (domyślnie 1000) |
| `FSED_SMOKE_ROW=N` | wybiera linię N, co przesuwa wideo jak kliknięcie |
| `FSED_SMOKE_ACTION=id` | uruchamia polecenie menu, np. `styleManager`, `fontCollector`, `macro:Strip tags/Strip tags` |
| `FSED_SMOKE_TOOL=N` | włącza narzędzie wizualne nr N (kolejność jak na pasku pod wideo) |
| `FSED_SMOKE_CLICK=tekst` | klika przycisk o tym napisie w otwartym dialogu |
| `FSED_SMOKE_DROP=plik` | upuszcza plik na pole edycji linii |
| `FSED_SMOKE_SET="klucz=wartość;..."` | ustawia opcje przed startem, np. `spell/lang=pl_PL` |

```bash
FSED_SMOKE=out.png FSED_SMOKE_MS=8000 FSED_SMOKE_ROW=1 fsed napisy.ass wideo.mkv
```

</details>

## Struktura projektu

```
crates/
├─ fs-core/        model dokumentu, parser i zapis ASS, tagi, czas, VFR, undo, operacje
├─ fs-formats/     SRT, MPL2, MicroDVD, TMP i wykrywanie kodowania
├─ fs-audio/       szczyty waveformu, spektrogram, odtwarzanie
├─ fs-media/       BestSource i libass (C++ przez FFI)
├─ fs-automation/  LuaJIT z API Automation 4 i dołączonymi skryptami Aegisuba
├─ fs-bridge/      most cxx między rdzeniem a Qt
└─ fs-cli/         binarka fsed
app/               GUI w Qt 6 Widgets (C++23, CMake)
docs/              architektura, opis CLI, notatki badawcze
tests/             korpus testowy i media testowe
tools/             instalacja zależności, generator mediów testowych, port vcpkg BestSource
```

## Dokumentacja

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): zasady, model dokumentu, formaty, media, automatyzacja i etapy prac.
- [docs/CLI.md](docs/CLI.md): wszystkie polecenia `fsed` z opcjami i przykładami.
- [docs/research/](docs/research): notatki o układzie Kainote, Automation 4 i wersjach zależności.
- [THIRD_PARTY.md](THIRD_PARTY.md): licencje zależności i dołączonych plików.

## Podziękowania

- [Aegisub](https://github.com/TypesettingTools/Aegisub): wzór logiki edytora oraz skrypty i biblioteki Automation 4 dołączone na ich licencjach.
- [Kainote](https://github.com/bjakja/Kainote): wzór układu interfejsu, odtworzony z zachowania programu, bez kopiowania kodu.
- [Tabler Icons](https://tabler.io/icons): ikony interfejsu.
