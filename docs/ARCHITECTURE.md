# FS Editor — plan architektury

FS Editor to wieloplatformowy (Windows, Linux, macOS) edytor napisów ASS/SSA.

* **Rdzeń** jest napisany w Rust (edycja 2024): model dokumentu, bezstratny parser i zapis ASS, formaty, undo, timecody VFR, operacje.
* **Interfejs** jest w Qt 6 Widgets (C++23, Qt ≥ 6.8). Wideo i wykresy audio rysuje GPU przez QRhi (`QRhiWidget`).
* **Most** Rust ↔ C++ jest zrobiony przez `cxx`. Rust buduje bibliotekę statyczną, a CMake łączy ją przez Corrosion.
* **Media**: BestSource dekoduje wideo i audio z dokładnością co do klatki, libass renderuje napisy, `cpal` odtwarza dźwięk, a `realfft` liczy spektrogram.
* **Automatyzacja** to LuaJIT przez `mlua`, z API zgodnym z Aegisub Automation 4 (karaskel, DependencyControl, MoonScript).
* **Układ UI** odwzorowuje Kainote, a **API i funkcje** wzorują się na Aegisub.
* **Licencja**: BSD-3-Clause.

## 1. Zasady

1. **Rdzeń nie zna GUI.** Cała logika edycji (operacje na liniach, przesuwanie czasów, filtrowanie, undo) żyje w Rust i jest testowalna z CLI oraz testów. GUI tylko wyświetla stan i wysyła komendy.
2. **Bezstratność.** Otwarcie i zapis pliku bez zmian daje identyczne bajty: BOM, końce linii, komentarze `;`, nieznane sekcje, `[Fonts]`/`[Graphics]`, `[Aegisub Project Garbage]`, `[Aegisub Extradata]`, kolejność kluczy i pól `Format:` oraz białe znaki zostają zachowane. Linia, której nikt nie dotknął, jest zapisywana z oryginalnego tekstu.
3. **Czas jest liczbą całkowitą.** Czas to `i64` w milisekundach, a ASS przechowuje centysekundy. Wideo VFR jest obsługiwane przez tablicę znaczników czasu klatek z semantyką Aegisub (`frame_at_time`, `time_at_frame` z trybami Start/End/Exact).
4. **Granice FFI są wąskie.** `cxx` eksportuje kilka nieprzezroczystych typów (`Project`, `Document`, `AudioEngine`, `VideoSource`, `Automation`) i proste struktury DTO, a nie cały model.
5. **Zależności są przypięte.** Rust używa `Cargo.lock` (commitowany), C++ używa `vcpkg.json` z `builtin-baseline` oraz `overrides`, a Qt ma wersję zapisaną w `CMakePresets.json` i `tools/qt-version.txt`.

## 2. Struktura repozytorium

```
fs-editor/
├─ Cargo.toml                 # workspace (edition 2024, resolver 3)
├─ Cargo.lock                 # przypięte wersje crate'ów
├─ rust-toolchain.toml        # stable, przypięta wersja
├─ LICENSE                    # BSD-3-Clause
├─ THIRD_PARTY.md             # licencje zależności i plików dołączonych (karaskel itd.)
├─ docs/
│  ├─ ARCHITECTURE.md         # ten dokument
│  ├─ CLI.md                  # instrukcja `fsed`
│  └─ research/               # notatki: układ Kainote, Automation 4, wersje zależności
├─ crates/
│  ├─ fs-core/                # model dokumentu, parser/zapis ASS, tagi, czas, kolory, undo, VFR, operacje
│  ├─ fs-formats/             # SRT, MPL2, MicroDVD, TMP, (SSA przez fs-core) + wykrywanie kodowania
│  ├─ fs-cli/                 # binarka `fsed` (info, check, roundtrip, convert, shift, sort, filter, …)
│  ├─ fs-audio/               # dekodowane PCM → cache szczytów (waveform), spektrogram (realfft), odtwarzanie (cpal)
│  ├─ fs-media/               # FFI do BestSource (wideo/audio) i libass (render) — C++ w crates/fs-media/cpp
│  ├─ fs-automation/          # LuaJIT (mlua): API `aegisub`, obiekt subtitles, dialogi, include'y, MoonScript, lfs, FFI exports
│  └─ fs-bridge/              # most cxx: staticlib + nagłówki dla Qt
├─ app/                       # GUI Qt 6 Widgets (C++23, CMake + Corrosion)
│  ├─ CMakeLists.txt, CMakePresets.json
│  ├─ src/
│  │  ├─ main.cpp             # start, smoke test (FSED_SMOKE*), raport awarii na Windows
│  │  ├─ bridge.h             # konwersje Qt ↔ cxx (QString ↔ rust::Str/String)
│  │  ├─ mainwindow.*         # menu, pasek narzędzi, karty na dole, pasek stanu, skróty, automatyzacja
│  │  ├─ tabpanel.*           # jedna karta = dokument + wideo + audio + editbox + grid + panel czasów
│  │  ├─ grid.*               # własny grid (QAbstractScrollArea), filtrowanie, przesuwanie czasów myszą
│  │  ├─ editbox.*            # pola linii, przyciski tagów, tryb tłumaczenia (2 pola)
│  │  ├─ shiftpanel.*         # panel „Zmiana czasów” i postprocesor
│  │  ├─ videobox.*           # VideoView (QRhiWidget), wątek dekodowania, zoom, tryb odtwarzacza
│  │  ├─ visualtools.*        # \pos \move \org \clip \iclip \p, skala, obroty
│  │  ├─ audiobox.*           # AudioView (QRhiWidget): waveform/spektrogram, karaoke, przyciski
│  │  ├─ rhiquad.*            # jeden teksturowany prostokąt QRhi używany przez wideo i audio
│  │  ├─ dialogs.*            # style, właściwości, szukaj/zamień, zaznacz linie, historia, ustawienia
│  │  └─ automationhost.*     # dialogi aegisub.dialog, wybór plików, postęp
│  └─ shaders/                # .vert/.frag → .qsb (qt_add_shaders)
├─ tests/
│  └─ corpus/                 # prawdziwe pliki .ass/.srt/… (z licencją pozwalającą na redystrybucję)
├─ vcpkg.json                 # ffmpeg, libass, bestsource (overlay port), xxhash — z baseline
└─ .github/workflows/ci.yml  # testy 3× OS, budowa GUI i paczki ZIP
```

## 3. Rdzeń (`fs-core`)

### 3.1 Model dokumentu

```text
Document
├─ source: SourceInfo        # BOM, kodowanie, dominujący koniec linii, czy plik kończył się newline
├─ sections: Vec<Section>     # kolejność jak w pliku
│   ├─ ScriptInfo  { header_raw, entries: Vec<InfoEntry> }      # klucz:wartość, komentarze ';', puste
│   ├─ Styles      { header_raw, format: FieldFormat, entries: Vec<StyleEntry> }
│   ├─ Events      { header_raw, format: FieldFormat, entries: Vec<EventEntry> }
│   └─ Other       { header_raw, lines: Vec<RawLine> }           # [Fonts], [Graphics], [Aegisub Project Garbage], nieznane
└─ (indeksy pomocnicze: styl→pozycja, id linii)
```

Każdy wpis (`StyleEntry`, `EventEntry`, `InfoEntry`) przechowuje:

* `raw: Option<RawLine>`, czyli oryginalny tekst linii i jej zakończenie (`\r\n`, `\n` lub brak), dopóki wpis jest „czysty”;
* sparsowane pola, czyli `Style`, `Event` albo `(key, value)`.

Zmiana pola czyści `raw`, a zapis serializuje wtedy wpis kanonicznie, według `Format:` sekcji i dominującego końca linii. Pola nieznane z `Format:` są przechowywane w `extra_fields`, żeby nie ginęły.

`Event`: `kind (Dialogue|Comment|Picture|Sound|Movie|Command)`, `layer`, `marked` (SSA), `start`, `end`, `style`, `actor`, `margin_l/r/v` (+ `margin_t/b` dla v4++), `effect`, `text`, a także `id` (stabilne, do undo i selekcji w GUI).

`Style`: wszystkie pola v4+ (nazwa, font, rozmiar, 4 kolory `&HAABBGGRR`, bold, italic, underline, strikeout, scale_x/y, spacing, angle, border_style, outline, shadow, alignment, marginesy, alpha_level, encoding) z konwersją v4 (SSA) ↔ v4+ (ASS), czyli wyrównanie numpad ↔ legacy.

### 3.2 Parser ASS

* Linie są dzielone z zachowaniem zakończeń, a BOM jest wykrywany (UTF-8 i UTF-16 przez `fs-formats::encoding`).
* Sekcje są rozpoznawane bez względu na wielkość liter. Linia `Format:` ustala kolejność pól, a domyślne formaty to v4+/v4/v4++.
* Ostatnie pole (`Text`) zawiera przecinki, a liczba podziałów wynika z liczby pól w `Format:`.
* Parser toleruje błędy tak jak libass/Aegisub: brakujące pola, spacje, ujemne czasy, `0:00:00.0`, godziny > 9. Nieparsowalne linie trafiają jako `RawLine` z ostrzeżeniem w `ParseReport`.

### 3.3 Tagi override (`fs-core::tags`)

Tokenizer bloków `{…}` daje `Text | Override(Vec<Tag>) | Comment | Drawing`. Rozpoznaje pełną listę tagów libass/VSFilter (`\pos \move \org \clip \iclip \fad \fade \t \k \kf \K \ko \kt \p \pbo \an \a \fn \fs \fsc \fscx \fscy \fsp \frx \fry \frz \fr \fax \fay \bord \xbord \ybord \shad \xshad \yshad \be \blur \c \1c-\4c \alpha \1a-\4a \b \i \u \s \q \r …`) i argumenty z tolerancją (np. `\pos(1,2` bez nawiasu). Z tokenizera korzystają narzędzia wizualne, karaoke, czyszczenie tagów, liczenie CPS i podmiana wartości (np. przesunięcie `\pos`). Zapis tagów jest minimalnie inwazyjny: zmienia się tylko dotknięty fragment tekstu.

### 3.4 Czas i VFR

* `Time(i64 ms)` z formatowaniem i parsowaniem ASS (`H:MM:SS.cc`), SRT (`HH:MM:SS,mmm`) i klatek.
* `FrameRate`:
  * `Cfr { num, den }`;
  * `Vfr { timestamps: Vec<i64> }` z timecodów v1/v2 albo z BestSource.
  * Operacje: `frame_at_time(ms, Start|End|Exact)` i `time_at_frame(frame, Start|End|Exact)` z semantyką Aegisub (czas startu linii = środek między klatkami), zapis timecodów v2 oraz ekstrapolacja za ostatnią klatką.
* Pliki keyframes: Aegisub (`# keyframe format v1`), XviD pass, x264 stats, SCXviD/WWXD.

### 3.5 Undo

Undo opiera się na transakcjach: `doc.edit("Opis", |tx| { … })`. Transakcja zbiera listę zmian odwracalnych (`InsertEvent`, `RemoveEvent`, `ModifyEvent{old,new}`, analogicznie dla stylów, `ScriptInfo` i całych sekcji). Historia ma dwa stosy i limit pamięci. „Amend” scala kolejne zmiany tego samego typu, np. pisanie w jednym polu. Każda transakcja zwraca `ChangeSet` (zakres dotkniętych wierszy), więc GUI odświeża tylko to, co trzeba. Undo stack jest per dokument (per karta).

### 3.6 Operacje (`fs-core::ops`)

Wszystkie operacje są wywoływalne z CLI, GUI i Lua:

* przesuwanie czasów (o czas lub o klatki, wszystkie/zaznaczone/od linii, start/koniec/oba, z ograniczeniem do stylów); w gridzie przesuwa się przeciągnięciem lub skrótami, jak w Kainote;
* sortowanie (start, koniec, styl, aktor, efekt, warstwa);
* filtrowanie (styl, aktor, efekt, tekst/regex, komentarze, zaznaczone, kolizje); filtr to widok, nie modyfikacja;
* szukaj/zamień (zwykły tekst/regex, pola, z/bez tagów);
* post-processor timingu (lead-in/out, łączenie sąsiednich, snap do keyframes);
* resample rozdzielczości (`PlayResX/Y`, `\pos`, `\clip`, rysunki, marginesy, rozmiary, obrys);
* zmiana FPS (CFR→CFR, VFR→CFR);
* kolizje i nakładanie, CPS/długość linii, czyszczenie tagów, łączenie i dzielenie linii;
* karaoke: podział `\k` na sylaby i ich zapis;
* tryb tłumaczenia: para (oryginał, tłumaczenie) per linia, odczyt/zapis w formacie zgodnym z Kainote (szczegóły w `docs/research/kainote-ui.md`).

### 3.7 Czcionki i pisownia

* `fs-core::fonts` zbiera kroje użyte w liniach (`\fn`, `\b`, `\i`, `\r`) razem ze znakami. Z funkcją `fontdb` szuka plików czcionek, także po nazwach GDI (ID 1/4/6), i wykrywa brakujące glify. Korzystają z tego `fsed fonts` i kolekcjoner czcionek w GUI, który przeszukuje też załączniki MKV.
* `fs-bridge::spell` opakowuje `spellbook` (słowniki Hunspell, także w kodowaniach typu ISO8859-2). Skaner słów pomija bloki `{…}`, `\N`/`\h` i rysunki `\p` i zwraca pozycje UTF-16 dla Qt.

## 4. Formaty (`fs-formats`)

| Format | Odczyt | Zapis | Uwagi |
|---|---|---|---|
| ASS (v4+, v4++) | ✔ | ✔ | bezstratnie (fs-core) |
| SSA (v4) | ✔ | ✔ | konwersja stylów/wyrównania |
| SRT | ✔ | ✔ | tagi `<i> <b> <u> <s> <font color>` ↔ override |
| MPL2 | ✔ | ✔ | `[ds][ds]tekst`, `/` = kursywa, `|` = nowa linia |
| MicroDVD | ✔ | ✔ | `{start}{end}`, `{1}{1}fps`, `{y:i}` itd., wymaga FPS |
| TMP | ✔ | ✔ | `hh:mm:ss:tekst` (czas końca liczony) |
| MPEG-4 Timed Text (TTXT) | ✔ | ✔ | XML GPAC, jak w Aegisub |
| Adobe Encore | – | ✔ | eksport, czasy SMPTE przy wybranym FPS |
| TranStation | – | ✔ | eksport, czasy SMPTE przy wybranym FPS |
| EBU STL (tech 3264) | – | ✔ | eksport binarny z ustawieniami Aegisub (standard TV, kodowanie, zawijanie, przesunięcie TC) |

Kodowanie: UTF-8/UTF-16 z BOM jest rozpoznawane deterministycznie, w pozostałych przypadkach używany jest `chardetng` z preferencją Windows-1250 dla polskich plików. Zapis domyślnie UTF-8 z BOM, jak w Aegisub, albo w kodowaniu źródłowym.

## 5. Media i audio

* **`fs-media` (C++ + cxx)**
  * `VideoSource`: BestSource `BestVideoSource`, z klatką RGBA dla dokładnego numeru klatki, tablicą timestampów (VFR), keyframes i rozdzielczością/SAR.
  * `AudioSource`: `BestAudioSource`, z próbkami PCM `f32`/`i16` dla zakresu próbek.
  * `SubtitleRenderer`: libass (`ass_read_memory` z aktualnego dokumentu), `render(time) → Vec<AssImage>`. Kompozycja odbywa się w shaderze (bitmapy alfa + kolor).
* **`fs-audio` (Rust)**
  * cache szczytów min/max per piksel w kilku poziomach zoomu (waveform);
  * spektrogram: STFT przez `realfft` (okno Hanna, rozmiar 2^n zależny od zoomu), wynik w dB jako tekstury kolumnowe;
  * odtwarzanie: `cpal` (WASAPI, ALSA/Pulse/PipeWire, CoreAudio), ring buffer, pozycja playheadu raportowana do GUI, resampling do częstotliwości urządzenia.
* **GUI**: `VideoWidget` i `AudioWidget` dziedziczą po `QRhiWidget`. Działa to na D3D11/D3D12, Vulkan i Metal, a OpenGL jest fallbackiem.

## 6. Automatyzacja (`fs-automation`)

* `mlua` z funkcjami `luajit` i `vendored`; lpeg kompilowany przez `cc` i rejestrowany jako `lpeg`.
* Globalna tablica `aegisub` z pełnym API Automation 4: `register_macro`, `register_filter`, `progress`, `debug`, `log`, `dialog`, `cancel`, `text_extents`, `frame_from_ms`, `ms_from_frame`, `video_size`, `keyframes`, `decode_path`, `file_name`, `project_properties`, `get_audio_selection`, `set_undo_point`, `gettext`, `lua_automation_version = 4`.
* Obiekt `subtitles` jako userdata z indeksowaniem 1..n po wszystkich liniach (info, style, eventy, format), `insert`, `append`, `delete`, `deleterange` i `[0]=`. Mapowanie na transakcję undo dokumentu.
* Pliki include z Aegisub (karaskel, utils, unicode, cleantags, `aegisub.re`/`aegisub.unicode`/`aegisub.util` itd.), redystrybuowane zgodnie z ich licencjami (BSD/ISC), opisane w `THIRD_PARTY.md`.
* Eksport symboli C wymaganych przez moduły FFI (regex, clipboard) i moduł `lfs`.
* Makra i filtry eksportu działają w wątku roboczym; okno postępu przenosi dialogi do GUI. Pierwsze „Anuluj” ustawia `is_cancelled` (jak w Aegisub), drugie („Zatrzymaj skrypt”) przerywa skrypt hookiem LuaJIT nawet wtedy, gdy nie sprawdza anulowania. Pętli w całości skompilowanej przez JIT, bez wywołań, hook nie przerwie.
* MoonScript: dołączony kompilator `moonscript` (Lua), który wymaga `lpeg`.
* DependencyControl: ścieżki `?user/automation/include`, `?user/automation/autoload`, `lfs`, `ffi`, `json`.
* Specyfikacja: `docs/research/automation4.md`.

## 7. GUI (Qt 6 Widgets) — układ Kainote

Szczegóły są w `docs/research/kainote-ui.md`. W skrócie:

* menu, pasek narzędzi i pasek kart (wiele plików naraz, każda karta ma własny dokument, wideo, audio i undo);
* karta zawiera wideo (lewa góra) i obok edytor linii; audio jest nad edytorem, a grid na dole;
* tryb tłumaczenia z dwoma polami tekstu w edytorze (oryginał i tłumaczenie) oraz kolumną oryginału w gridzie;
* przesuwanie czasów w gridzie, panel przesuwania czasów i filtrowanie linii;
* zoom wideo i tryb odtwarzacza (samo wideo bez edycji, pełny ekran).

## 8. Etapy

| Etap | Zakres | Kryterium ukończenia |
|---|---|---|
| 0 | Plan, struktura repo, licencja, toolchain | ten dokument, `cargo check` |
| 1 | `fs-core` + `fs-formats` + `fsed` CLI | testy roundtrip bajt-w-bajt na prawdziwych plikach, testy jednostkowe czasu/VFR/undo/tagów/formatów |
| 2 | `fs-audio` (peaks, FFT, cpal) + `fs-media` (BestSource, libass) | testy na wygenerowanym audio/wideo (seek co do klatki, VFR, audio co do próbki, libass), `fsed media info`, `fsed media frame` |
| 3 | `fs-automation` (Automation 4, karaskel, MoonScript, DependencyControl) | uruchomienie prawdziwych skryptów (kara-templater, skrypty z TypesettingTools) w `fsed lua` |
| 4 | `fs-bridge` + GUI Qt | aplikacja z pełnym układem Kainote, wideo i audio na QRhi |
| 5 | Pakowanie (Windows installer, AppImage, .dmg), CI 3× OS | artefakty z GitHub Actions |
