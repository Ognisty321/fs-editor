# FS Editor

Wieloplatformowy (Windows, Linux, macOS) edytor napisów ASS/SSA.

| Warstwa | Technologia |
|---|---|
| Rdzeń | Rust (edycja 2024): model dokumentu, bezstratny parser ASS, undo, VFR |
| Interfejs | Qt 6 Widgets (C++23), wideo i audio przez QRhi |
| Most | `cxx` |
| Media | BestSource, libass, cpal, realfft |
| Automatyzacja | LuaJIT przez `mlua`, zgodność z Aegisub Automation 4 |

Układ interfejsu wzoruje się na Kainote, a funkcje i API na Aegisub.

Szczegóły i etapy prac są w [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Stan

| Etap | Zakres | Stan |
|---|---|---|
| 1 | `fs-core`, `fs-formats`, CLI `fsed` | gotowy |
| 2 | `fs-audio`, `fs-media` | gotowy |
| 3 | `fs-automation`, `fsed lua` | gotowy |
| 4 | most `cxx` i GUI Qt | gotowy |
| 5 | paczki ZIP (CPack) i CI | Windows i Linux zbudowane i spakowane lokalnie; macOS i workflow CI nieuruchomione |

## Budowanie rdzenia i CLI

Potrzebny jest Rust w wersji z `rust-toolchain.toml`. `rustup` pobierze ją sam.

```bash
cargo build --release -p fs-cli
```

```bash
cargo test --workspace
```

Opis poleceń `fsed` jest w [docs/CLI.md](docs/CLI.md).

`fs-automation` buduje LuaJIT ze źródeł (`mlua`, vendored), więc potrzebny jest kompilator C (MSVC, GCC albo Clang). Na Windows budowanie LuaJIT przerywa zmienna `NoDefaultCurrentDirectoryInExePath`, bo `msvcbuild.bat` uruchamia `minilua` z bieżącego katalogu. Jeśli jest ustawiona, usuń ją na czas budowania:

```bash
env -u NoDefaultCurrentDirectoryInExePath cargo test --workspace
```

Test zgodności z DependencyControl uruchamia się po wskazaniu jego repozytorium:

```bash
FS_DEPCTRL_DIR=/ścieżka/do/DependencyControl cargo test -p fs-automation dependency_control
```

## Budowanie z obsługą wideo i audio

Crate `fs-media` (BestSource, FFmpeg, libass) potrzebuje bibliotek C/C++ z vcpkg. Skrypt instaluje je z [vcpkg.json](vcpkg.json) poza repozytorium, bo port FFmpeg odrzuca ścieżki ze spacjami, i zapisuje ich położenie w `.cargo/config.toml`.

Windows (vcpkg dołączony do Visual Studio albo `VCPKG_ROOT`):

```bash
pwsh tools/deps.ps1
```

Linux i macOS (wymaga `VCPKG_ROOT`):

```bash
tools/deps.sh
```

Potem:

```bash
cargo test -p fs-media
```

```bash
cargo build --release -p fs-cli --features media
```

Media testowe w `tests/media` generuje `tools/gen-test-media.sh` (potrzebne ffmpeg z libx264 i mkvmerge). Każda klatka jest jednolicie szara, a jej jasność koduje numer klatki, więc testy sprawdzają dokładność seeków po jednym pikselu.

## Budowanie GUI

Potrzebne są Qt 6.8 lub nowsze (z modułem Qt Shader Tools), CMake 3.28+, zależności z `tools/deps` oraz Rust. Most `fs-bridge` buduje cargo przez Corrosion.

Windows (Visual Studio 2026):

```bash
cd app && cmake --preset windows -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DFS_DEPS_DIR="$LOCALAPPDATA/fs-editor/deps/installed/x64-windows"
```

```bash
cd app && cmake --build --preset windows
```

Preset `windows-dev` buduje rdzeń szybszym profilem cargo `gui-dev`. Jeśli ustawiona jest zmienna `NoDefaultCurrentDirectoryInExePath`, usuń ją jak przy `cargo test`.

Linux (sprawdzone na Ubuntu 26.04, GCC, Qt 6.12 z aqtinstall) i macOS używają presetu `unix` (Ninja):

```bash
TRIPLET=x64-linux-dynamic tools/deps.sh
```

```bash
cd app && cmake --preset unix -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64 -DFS_DEPS_DIR=$HOME/.cache/fs-editor/deps/installed/x64-linux-dynamic && cmake --build build-unix
```

Na Linuksie vcpkg potrzebuje m.in. `nasm`, `zip`, `gperf`, `bison`, `flex`, `autoconf-archive`, a cpal nagłówków ALSA (`libasound2-dev`). `tools/deps.sh` każe budować PCRE2 ze źródeł (`PCRE2_SYS_STATIC`), bo systemowe PCRE2 z pkg-config dokłada `-L/usr/lib/...`, przez co linker brałby systemowe FFmpeg zamiast tego z vcpkg. Paczka ZIP zawiera Qt i biblioteki mediów (RUNPATH `$ORIGIN/../lib`). Z systemu potrzebne są tylko biblioteki X11/xcb wymagane przez Qt (`libxcb-cursor0`, `libxcb-util1`, `libxcb-xkb1`, `libxkbcommon-x11-0` i pokrewne), które są na zwykłym pulpicie Linuksa.

Paczka ZIP z Qt, bibliotekami mediów i skryptami automatyzacji:

```bash
cd app/build-windows && cpack -C Release
```

### Smoke test GUI

`fsed` w trybie GUI czyta zmienne środowiskowe, które pozwalają sprawdzić aplikację bez klikania:

* `FSED_SMOKE=plik.png` zapisuje zrzut okna (albo otwartego dialogu modalnego) i zamyka program;
* `FSED_SMOKE_MS` to czas do zrzutu (domyślnie 1 s);
* `FSED_SMOKE_ROW=N` wybiera linię N, co przesuwa wideo jak kliknięcie;
* `FSED_SMOKE_ACTION=id` uruchamia polecenie menu, np. `styleManager`, `fontCollector` albo `macro:Strip tags/Strip tags`;
* `FSED_SMOKE_TOOL=N` włącza narzędzie wizualne nr N (kolejność jak na pasku pod wideo);
* `FSED_SMOKE_CLICK=tekst` klika przycisk o tym napisie w otwartym dialogu, np. `Check`;
* `FSED_SMOKE_SET="klucz=wartość;..."` ustawia opcje przed startem, np. `spell/dir=...;spell/lang=pl_PL`.

Smoke test używa osobnej gałęzi ustawień („FS Editor smoke test”), więc nie rusza twojej konfiguracji.

```bash
FSED_SMOKE=out.png FSED_SMOKE_MS=8000 FSED_SMOKE_ROW=1 fsed napisy.ass wideo.mkv
```

## Sprawdzanie pisowni

Edytor podkreśla błędy w polu tekstu; tagi, `\N` i rysunki `\p` są pomijane. Prawy przycisk myszy pokazuje podpowiedzi i „Dodaj do słownika”. Słowniki Hunspell (`xx_YY.aff` + `xx_YY.dic`, także w kodowaniach typu ISO8859-2) wrzuca się do `dictionaries/` obok `fsed` albo wskazuje folder i język w ustawieniach. Najnowsze słowniki: polski z [sjp.pl](https://sjp.pl/sl/en/), angielski z [en-wl/wordlist](https://github.com/en-wl/wordlist/releases).

Testy na prawdziwych słownikach dostają ścieżkę bez rozszerzenia:

```bash
FS_DICT=/sciezka/pl_PL FS_DICT_EN=/sciezka/en_US cargo test -p fs-bridge spell
```

## Kolekcjoner czcionek

Plik → Kolekcjoner czcionek (albo `fsed fonts`) zbiera kroje użyte w liniach i szuka ich w czcionkach systemowych, w wybranym folderze i w załącznikach otwartego wideo MKV. Zgłasza brakujące czcionki, brakujące glify i sztuczne pogrubienie lub kursywę, a na życzenie kopiuje pliki do wskazanego folderu.

## Testy na prawdziwych plikach

Testy korpusowe ładują pliki z trzech miejsc:

* `tests/corpus/`: pliki testowe Aegisub, timecody VFR, keyframe'y i próbki kodowań;
* `tests/corpus-local/`: twoje własne pliki `.ass`; katalog jest w `.gitignore`;
* katalogu wskazanego przez zmienną `FS_CORPUS_DIR`.

Dla każdego pliku testy sprawdzają:

* czy odczyt i zapis odtwarza plik bajt w bajt;
* czy zmiana jednego pola nie rusza pozostałych;
* czy tagi override są parsowane bezstratnie;
* czy konwersja przez SRT, MPL2, MicroDVD, TMP i SSA i z powrotem zachowuje linie.

## Licencja

BSD-3-Clause, zob. [LICENSE](LICENSE). Licencje zależności i dołączonych plików są w [THIRD_PARTY.md](THIRD_PARTY.md).
