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
│  │  ├─ colorpicker.*        # okno koloru Aegisuba: widmo w 5 trybach, RGB/HSL/HSV, ASS/HTML, alfa, ostatnie kolory, pipeta
│  │  ├─ theme.*              # motywy kolorów: role kolorów, Ciemny/Jasny, motywy użytkownika (.ini), paleta okien
│  │  ├─ hydra.*              # lista tagów narzędzia „wszystkie tagi” (Hydra Kainote) i jej edytor
│  │  ├─ associations.*       # skojarzenia typów plików napisów (rejestr użytkownika w Windows)
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

Podświetlanie składni (`fs-core::syntax`) to port lexera Aegisuba (`TokenizeDialogueBody`, `MarkDrawings`, `SyntaxHighlight`) jako ręcznie pisany automat z tą samą regułą najdłuższego dopasowania. Rozróżnia nawiasy, ukośniki i nawiasy okrągłe, nazwy tagów, parametry, komentarze, łamania linii, kod i zmienne szablonu karaoke (tylko w linijkach-szablonach). Rysunki (`\p` > 0 i wektorowy `\clip`) dzieli na polecenia, współrzędne X/Y i punkty końcowe krzywych, a błędy oznacza (brak `m` na początku, nieparzysta liczba współrzędnych, polecenie sklejone z liczbą). Wynikiem są przebiegi (styl, długość). Most zwraca je w jednostkach UTF-16 dla `QSyntaxHighlighter`. Testy Aegisuba są przeniesione 1:1.

### 3.4 Czas i VFR

* `Time(i64 ms)` z formatowaniem i parsowaniem ASS (`H:MM:SS.cc`), SRT (`HH:MM:SS,mmm`) i klatek.
* `FrameRate`:
  * `Cfr { num, den }`;
  * `Vfr { timestamps: Vec<i64> }` z timecodów v1/v2 albo z BestSource.
  * Operacje: `frame_at_time(ms, Start|End|Exact)` i `time_at_frame(frame, Start|End|Exact)` z semantyką Aegisub (czas startu linii = środek między klatkami), zapis timecodów v2 oraz ekstrapolacja za ostatnią klatką.
* Pliki keyframes: Aegisub (`# keyframe format v1`), XviD pass, x264 stats, SCXviD/WWXD.

### 3.5 Undo

Undo opiera się na transakcjach: `doc.edit("Opis", |tx| { … })`. Transakcja zbiera listę zmian odwracalnych (`InsertEvent`, `RemoveEvent`, `ModifyEvent{old,new}`, analogicznie dla stylów, `ScriptInfo` i całych sekcji). Historia ma dwa stosy, limit kroków i budżet pamięci (512 MB). Krok kosztuje tylko linie, których nie współdzieli ze stanem po nim (dokument to `Arc`-owane linie kopiowane przy zapisie), więc sortowanie 50 000 linii jest prawie darmowe, a zmiana każdej linii kosztuje cały plik. Najstarsze kroki odpadają ponad budżet, ale 20 ostatnich zostaje zawsze. „Amend” scala kolejne zmiany tego samego typu, np. pisanie w jednym polu. Każda transakcja zwraca `ChangeSet` (zakres dotkniętych wierszy), więc GUI odświeża tylko to, co trzeba. Undo stack jest per dokument (per karta).

### 3.6 Operacje (`fs-core::ops`)

Wszystkie operacje są wywoływalne z CLI, GUI i Lua:

* przesuwanie czasów (o czas lub o klatki, wszystkie/zaznaczone/od linii, start/koniec/oba, z ograniczeniem do stylów); w gridzie przesuwa się przeciągnięciem lub skrótami, jak w Kainote;
* sortowanie (start, koniec, styl, aktor, efekt, warstwa);
* filtrowanie (styl, aktor, efekt, tekst/regex, komentarze, zaznaczone, kolizje); filtr to widok, nie modyfikacja. Linijki ukryte ręcznie i te, które filtr zostawia ukryte („nie resetuj”), są zapamiętane po identyfikatorach, więc wstawienie lub usunięcie linijek przed nimi ich nie przesuwa;
* szukaj/zamień (zwykły tekst/regex, pola, z/bez tagów). Zamiana z regex czyta odwołania jak Aegisub (format Perla z boost): `$1`, `${1}`, `\1`–`\9` i `\\` jako jeden backslash. Wyjątkiem są tagi koloru i alfy (`\1c`, `\3a`), które zostają tagami, oraz `\N`, który zostaje łamaniem linii (Aegisub zostawia z niego samo „N”);
* konwersja formatu jest krokiem undo razem z formatem i nazwą pliku: cofnięcie przywraca linijki, format i ścieżkę;
* post-processor timingu (lead-in/out, łączenie sąsiednich, snap do keyframes);
* resample rozdzielczości (`PlayResX/Y`, `\pos`, `\clip`, rysunki, marginesy, rozmiary, obrys);
* zmiana FPS (CFR→CFR, VFR→CFR);
* kolizje i nakładanie, CPS/długość linii (komórka CPS w gridzie czerwienieje stopniowo od progu ostrzeżenia do progu błędu, jak w Aegisub), czyszczenie tagów, łączenie i dzielenie linii;
* karaoke: podział `\k` na sylaby i ich zapis;
* zmiana jednego tagu w wielu linijkach (`tag_adjust`, Hydra Kainote i „zmiana wszystkich tagów” narzędzi skali i obrotu): dodanie, ustawienie, mnożenie przez numer linijki, gradienty, wstawienie (w miejscu kursora albo na początku linijki; dla `\t` wokół zaznaczonych tagów), krok od aktywnej linijki oraz skalowanie wszystkich wystąpień. Tag ma 1–4 wartości: alfa jedną, kolor trzy (R, G, B), `\pos`, `\fad` i `\t` swoje początkowe liczby. Zmiany względne obejmują też tagi wewnątrz `\t`, a linijka bez tagu na początku dostaje go tam (od wartości stylu), żeby zmieniła się cała. Liczone są zawsze od tekstu sprzed przeciągania, więc nic się nie kumuluje;
* „utrzymanie proporcji” (`transform_place`): pozycje, oba punkty `\move`, `\org` i clipy przekształcone tą samą skalą lub obrotem wokół jednego punktu. Przy obrocie linijka z `\org` przesuwa tylko `\org`, bo libass już obraca ją wokół niego;
* tryb tłumaczenia: para (oryginał, tłumaczenie) per linia, odczyt/zapis w formacie zgodnym z Kainote (szczegóły w `docs/research/kainote-ui.md`). Łączenie scala oryginały z oryginałami i tłumaczenia z tłumaczeniami (wiersz bez tłumaczenia wnosi pusty tekst), a czasy bierze z tłumaczeń. Dzielenie tnie połówkę, w której stoi kursor, a druga zostaje w całości w pierwszej części. Drzewa wstawiają opis przed oryginałem pary, a znacznik dostają obie połówki, więc zostaje po wyłączeniu trybu. „Rekombinacja” i podział karaoke działają tylko poza trybem, bo zależą od wspólnego tekstu linijek, którego oryginał i tłumaczenie nie mają.

### 3.7 Czcionki i pisownia

* `fs-core::fonts` zbiera kroje użyte w liniach (`\fn`, `\b`, `\i`, `\r`) razem ze znakami. Z funkcją `fontdb` szuka plików czcionek, także po nazwach GDI (ID 1/4/6), i wykrywa brakujące glify. Korzystają z tego `fsed fonts` i kolekcjoner czcionek w GUI, który przeszukuje też załączniki MKV.
* folder czcionek zewnętrznych (Kainote): czcionki z niego renderuje libass (`ass_set_fonts_dir`, dla wideo otwartego po ustawieniu), widać je na listach czcionek (Qt), a kolektor szuka w nim jak w systemowych;
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

Każdy importowany format (SRT, MicroDVD, MPL2, TMP, TTXT, zwykły tekst, też ścieżka z MKV) ma w ustawieniach własny katalog stylów menedżera, jak w Aegisub: wczytane napisy dostają jego style (domyślnie katalog „Default”), więc tak wyglądają na wideo i po konwersji do ASS. Media zapisane w skrypcie otwierają się z nim zawsze, nigdy albo po pytaniu z listą plików (Aegisub App/Auto/Load Linked Files); to, co już jest otwarte, zostaje.

Kodowanie: UTF-8/UTF-16 z BOM jest rozpoznawane deterministycznie, w pozostałych przypadkach używany jest `chardetng` z preferencją Windows-1250 dla polskich plików. Zapis domyślnie UTF-8 z BOM, jak w Aegisub, albo w kodowaniu źródłowym.

## 5. Media i audio

* **`fs-media` (C++ + cxx)**
  * `VideoSource`: BestSource `BestVideoSource`, z klatką RGBA dla dokładnego numeru klatki, tablicą timestampów (VFR), keyframes i rozdzielczością/SAR.
  * `AudioSource`: `BestAudioSource`, z próbkami PCM `f32`/`i16` dla zakresu próbek. Plik z kilkoma ścieżkami audio wczytuje pierwszą pasującą do preferowanych języków lub nazw z ustawień (Kainote, `fs-bridge::media::preferred_track`), a bez dopasowania pyta, którą wczytać, jak Aegisub.
  * `SubtitleRenderer`: libass (`ass_read_memory` z aktualnego dokumentu), `render(time) → Vec<AssImage>`. Kompozycja odbywa się w shaderze (bitmapy alfa + kolor). GUI przekazuje wątkowi renderu kopię dokumentu w O(1) (`RenderSubs`). Zapis skryptu dla libass i rejestracja czcionek z `[Fonts]` odbywają się w tym wątku, nie w GUI.
* **`fs-audio` (Rust)**
  * cache szczytów min/max per piksel w kilku poziomach zoomu (waveform). Sumy |x| poziomu są dzielone przez 4^poziom, więc mieszczą się w `u32` na każdym poziomie. Dekodowane PCM trafia od razu do współdzielonego `Arc<[i16]>`, bez drugiej kopii ścieżki. Puste audio (2h30m jak w Aegisub) ma 1 kHz: 18 MB zamiast 800;
  * spektrogram: STFT przez `realfft` (okno Hanna, rozmiar 2^n zależny od zoomu), wynik w dB jako tekstury kolumnowe. Próbkowane kolumny leżą na wielokrotnościach jednego kroku dla całego widoku, więc przewinięty widok czyta kolumny z cache. Brakujące liczą się na wszystkich rdzeniach, a cache (64 MB) usuwa najdawniej czytane;
  * odtwarzanie: `cpal` (WASAPI, ALSA/Pulse/PipeWire, CoreAudio), ring buffer, pozycja playheadu raportowana do GUI, resampling do częstotliwości urządzenia; urządzenie wyjścia można wybrać w ustawieniach (brakujące wraca do domyślnego). Opóźnienie audio (Kainote) przesuwa próbki raz przy wczytaniu (`PcmBuffer::delayed`): cisza na początku albo obcięty początek, więc fala, widmo i odtwarzanie zgadzają się bez dalszych przeliczeń.
* **GUI**: `VideoWidget` i `AudioWidget` dziedziczą po `QRhiWidget`. Działa to na D3D11/D3D12, Vulkan i Metal, a OpenGL jest fallbackiem.

## 6. Automatyzacja (`fs-automation`)

* `mlua` z funkcjami `luajit` i `vendored`; lpeg kompilowany przez `cc` i rejestrowany jako `lpeg`. luabins (C, z podprojektu Aegisub) też jest kompilowany przez `cc`; `require "luabins"` najpierw pokazuje ostrzeżenie Aegisub o przestarzałej bibliotece, a potem zwraca prawdziwy moduł.
* Globalna tablica `aegisub` z pełnym API Automation 4: `register_macro`, `register_filter`, `progress`, `debug`, `log`, `dialog`, `cancel`, `text_extents`, `frame_from_ms`, `ms_from_frame`, `video_size`, `keyframes`, `decode_path`, `file_name`, `project_properties`, `get_audio_selection`, `set_undo_point`, `gettext`, `lua_automation_version = 4`.
* Obiekt `subtitles` jako userdata z indeksowaniem 1..n po wszystkich liniach (info, style, eventy, format), `insert`, `append`, `delete`, `deleterange` i `[0]=`. Mapowanie na transakcję undo dokumentu.
* Pliki include z Aegisub (karaskel, utils, unicode, cleantags, `aegisub.re`/`aegisub.unicode`/`aegisub.util` itd.), redystrybuowane zgodnie z ich licencjami (BSD/ISC), opisane w `THIRD_PARTY.md`.
* Eksport symboli C wymaganych przez moduły FFI (regex, clipboard) i moduł `lfs`.
* Okno postępu działa jak `DialogProgress` z Aegisub: `aegisub.debug.out` i błędy skryptu pojawiają się w nim na bieżąco, a jeśli skrypt coś napisał, okno zostaje otwarte z przyciskiem „Zamknij”. Tekst spoza uruchomienia (np. błąd funkcji `validate`) trafia od razu do okna logu.
* Menu Automatyzacji przy otwarciu pobiera stan wszystkich makr jednym wywołaniem: zaznaczenie i kontekst projektu liczą się raz. `validate` może zastąpić opis w pasku stanu, a makro z `is_active` jest pozycją z zaznaczeniem (COMMAND_TOGGLE z Aegisub).
* `aegisub.video_size` zwraca proporcje i ich tryb jak Aegisub (Video AR Mode/Value projektu; wideo z niekwadratowymi pikselami to tryb własny). `text_extents` trzyma DC i czcionki w wątku zamiast tworzyć je przy każdym wywołaniu.
* Makra i filtry eksportu działają w wątku roboczym; okno postępu przenosi dialogi do GUI. Pierwsze „Anuluj” ustawia `is_cancelled` (jak w Aegisub), drugie („Zatrzymaj skrypt”) przerywa skrypt hookiem LuaJIT nawet wtedy, gdy nie sprawdza anulowania. Pętli w całości skompilowanej przez JIT, bez wywołań, hook nie przerwie.
* Eksport jak `DialogExport` z Aegisub. Łańcuch filtrów tworzą wbudowane „Transform Framerate” (priorytet 1000) i „Fix Styles” (−5000) oraz filtry skryptów w kolejności wczytania, posortowane malejąco po priorytecie; powtórzona nazwa dostaje „ (N)”. Okno dostaje panele konfiguracji wszystkich filtrów od razu (w wątku roboczym). Lista zaznaczonych filtrów (w kolejności z listy) trafia do pliku jako „Export Filters”, nawet po anulowaniu, a kodowanie jako „Export Encoding”. Wartości paneli skryptów są pamiętane w sesji dokumentu w formacie Aegisub (`nazwa:wartość|…`, zakodowane jak `inline_string_encode`), nie w pliku. Filtry działają na kopii dokumentu w wybranej kolejności.
* Skrypty lokalne pliku („Automation Scripts”, `LocalScriptManager` z Aegisub): znaczniki `~` (folder pliku), `$` (folder automatyzacji) i `/` (ścieżka bezwzględna). Przy otwarciu pliku program pyta, czy ufasz autorom. Nieznany znacznik i brakujący plik dają ostrzeżenia Aegisub. Skrypt dodany ręcznie trafia na listę pliku pod najkrótszą z tych postaci, a usunięty w menedżerze z niej znika.
* Skrypty per plik, jak okna Aegisuba: skrypty z autoload służą wszystkim kartom, a pozostałe tylko plikom, które mają je na liście „Automation Scripts”. Menu, eksport i menedżer karty pokazują tylko jej skrypty. Makra skryptu innego pliku nie da się uruchomić. Zamknięcie karty usuwa skrypty, których nie ma już żaden otwarty plik.
* Autoload wczytuje skrypty równolegle, jak Aegisub (`std::async`): jeden stan Lua na skrypt, pula wątków o rozmiarze `available_parallelism`. Kolejność menu zostaje kolejnością plików, a skrypt, który spowoduje panikę, jest zgłaszany i pomijany.
* `Automation/Trace Level` (Ustawienia → Automatyzacja) to wspólny atomik czytany przy każdym `aegisub.debug.out`, więc zmiana działa też w skryptach już wczytanych.
* MoonScript: dołączony kompilator `moonscript` (Lua), który wymaga `lpeg`.
* DependencyControl: ścieżki `?user/automation/include`, `?user/automation/autoload`, `lfs`, `ffi`, `json`.
* Specyfikacja: `docs/research/automation4.md`.

## 7. GUI (Qt 6 Widgets) — układ Kainote

Szczegóły są w `docs/research/kainote-ui.md`. W skrócie:

* menu, pasek narzędzi i pasek kart (wiele plików naraz, każda karta ma własny dokument, wideo, audio i undo). Kilka plików otwartych naraz (upuszczonych, z wiersza poleceń albo z okna otwierania) łączy się jak w Kainote: napisy i wideo w kolejności nazw („ep2” przed „ep10”) tworzą pary, każda w osobnej karcie, a audio i klatki kluczowe trafiają do ostatniej;
* karta zawiera wideo (lewa góra) i obok edytor linii; audio jest nad edytorem, a grid na dole;
* ustawienia konwersji z Kainote (`Session::convert_with`, jeden krok cofania): do ASS rozdzielczość wideo albo nowych plików (u Kainote stałe 1280×720), styl z katalogu w miejsce Default, tagi na początku linii; nowe czasy końca według liczby znaków bez tagów (min. 1 s, przycięte do następnej linii; TMP zawsze); FPS MicroDVD bez wideo; okno ustawień przed konwersją;
* dalsze opcje Kainote: pełny ekran dla samego wideo przy starcie, bez ostrzeżeń narzędzi wizualnych, linia widziana na pełnym ekranie i linia z czasem poprzedniej karty jako aktywne (linia z tym czasem albo najbliższy początek), sortowanie przy otwieraniu (krok cofania tylko przy zmianie kolejności; posortowane nie dostaje pustego kroku), wartości przesuwania czasów przechodzące między kartami;
* zachowania z Kainote (opcje, domyślnie jak w Aegisubie): napisy w bieżącej karcie, Enter w czasach bez przejścia do następnej linii, podpowiedzi pisowni po dwukliku, wyśrodkowana aktywna linia przy skokach (nie przy klikach i strzałkach), oryginały zaznaczone po duplikowaniu, pauza lewym przyciskiem, wideo od aktywnej linii, własny znak uproszczonych tagów, długość tytułu karty i liczba plików autozapisu;
* zachowanie z Aegisuba: narzędzia wizualne rysowane tylko z myszą nad wideo, klatki kluczowe w audio osobno w trybie karaoke, kliknięcie w siatce bez zabierania fokusu polu edycji;
* limity z Aegisuba: poziomy cofania (na sesję, zostają po otwarciu innego pliku; nadal obok budżetu pamięci) i liczba wpisów historii wyszukiwania i zamiany;
* opcje wyświetlania z Aegisuba: styl fali (samo maksimum albo maksimum ze średnią), czcionka sylab karaoke (pasek sylab rośnie z nią), klatki kluczowe na pasku przewijania do wyłączenia, akcje kółka na wideo osobno dla zwykłego, z Ctrl i z Shift (rozmiar okna, zoom, przesuwanie, każde też odwrotnie, albo nic) i miejsce zapisu klatek (obok wideo, obok napisów albo pytanie o folder);
* automatyzacja: foldery autoload i include do ustawienia jak w Aegisub (lista rozdzielona `|`, tokeny `?user`/`?data`; pusta to standardowy układ), automatyczne przeładowanie zmienionych skryptów (żadne / lokalne / globalne / wszystkie; `QFileSystemWatcher` odświeżany z menu automatyzacji) i przycisk „Edytuj” w menedżerze jak w Kainote (edytor z ustawień albo program systemu);
* pole edycji: podpowiedzi parametrów tagu jak w Aegisub (`fs_core::calltip` na tokenach z `syntax`; jego błąd o jeden, gubiący przecinek zaraz za kursorem przy wyborze przeciążenia, poprawiony; dymek pogrubia parametr pod kursorem) i dopasowanie nawiasów `{}`/`()` jak w Kainote (kolor tła w motywie); zrzut testu dymnego dorysowuje widoczne dymki i popupy;
* spektrum jak w Aegisub: jakość (cztery poziomy, rozmiar FFT i krok jak w wersji z FFTW), skala częstotliwości (pięć krzywych między liniową a logarytmiczną, z 1 kHz na zadanej wysokości; przycisk wzmocnienia mowy dalej daje własną skalę logarytmiczną), schematy kolorów Icy Blue i Green (HSL liczone w shaderze) i limit pamięci podręcznej kolumn;
* okno ustawień z „Zastosuj” i „Przywróć domyślne” jak w Aegisub (przywraca opcje swoich stron i otwiera się na nowo; motywy, skróty i skojarzenia plików mają własne przywracanie), zbieracz czcionek kopiuje też do folderu napisów;
* kopie zapasowe jak w Aegisub: folder autozapisu do wyboru, kopia .ORIGINAL otwieranego pliku do wyłączenia i zapis samego pliku po każdej zmianie (zmiany w odstępie do 300 ms zapisują się raz; tylko plik, który zapisuje się bez pytań);
* drobne opcje Aegisuba: zapis stanu interfejsu w pliku (aktywna linijka, przewinięcie, pozycja wideo; wyłączony usuwa je z pliku), podświetlanie składni w polu edycji, Shift+Enter jako `\n` zamiast `\N`, rozdzielczość nowych plików (z pierwszego wideo albo stała) i rozmiar ikon paska narzędzi;
* opcje audio z Aegisuba i Kainote: co jest rysowane (klatki kluczowe, czas pod kursorem, pozycja wideo, tło zaznaczenia, granice sekund), grubość granic linijki, odległość przyciągania, czułość przeciągania i czas odtwarzany wokół znacznika;
* tryb tłumaczenia z dwoma polami tekstu w edytorze (oryginał i tłumaczenie) oraz kolumną oryginału w gridzie;
* przesuwanie czasów w gridzie, panel przesuwania czasów i filtrowanie linii;
* zoom wideo (kółko, prostokąt, komendy Aegisub: powiększ/pomniejsz Ctrl+Num+/−, 50/100/200% pikseli wideo, „Ctrl+Spacja” na suwak) i tryb odtwarzacza (samo wideo bez edycji, pełny ekran). Oddalenie poniżej dopasowania rysuje całą klatkę mniejszą (do 10%) na środku widoku, a „Wyśrodkuj powiększone wideo” (Aegisub video/reset_pan) przesuwa powiększony fragment na środek klatki bez zmiany zoomu. Krok szybkiego skoku (Alt+←/→) jest w ustawieniach wideo, domyślnie 10 klatek;
* kolory wideo jak w Aegisub: klatki są dekodowane macierzą z nagłówka `YCbCr Matrix` skryptu (None to macierz samego wideo, brak lub błędna wartość to TV.601 VSFiltera). Przy otwarciu wideo program ostrzega o wideo bez oznaczonej macierzy i o wideo HDR/WCG, a potem ustawia albo proponuje macierz i rozdzielczość układu (`LayoutResX/Y`) skryptu według ustawień wideo, z zapamiętanym ostatnim wyborem. Nowy, nietknięty plik dostaje je bez pytania;
* komendy Aegisub z własnymi klawiszami w edytorze skrótów: przesuwanie linii (Alt+↑/↓; w polu tekstu Alt+↓ nadal oznacza linię jako niepotwierdzoną), widok widma lub fali audio, Ctrl+Enter w polu tekstu (zatwierdza czasy z audio i zamyka krok undo bez zmiany linii), kopiowanie, wycinanie i wklejanie linijek, pokazanie lub ukrycie paska narzędzi i każdy tryb wycinka wektorowego. Komenda może mieć dowolnie wiele klawiszy (jak w Aegisub); klawisz przejęty od innej komendy znika tylko z jej listy;
* tryb Medusa (Aegisub app/toggle/global_hotkeys, menu Audio): klawisze timingu, które nic nie wpisują (klawiatura numeryczna, z Ctrl lub Alt, klawisze F), działają w całym oknie, poza wideo i audio, które mają własne;
* rysunek odręczny wygładzony (Aegisub): punkt co 60 pikseli, a po puszczeniu przycisku figura zamienia się w krzywe Béziera (`fs-bridge::visual::smooth_figure`, algorytm Shemanareva z `Spline::Smooth`, figura zamknięta);
* asystent tłumaczenia (Aegisub, menu Napisy): bloki zwykłego tekstu linijek po kolei od aktywnej (bez tagów, komentarzy, rysunków i samych spacji, `fs-core::tags::translatable_ranges`), oryginał z wyróżnionym blokiem u góry, a wpisane tłumaczenie zastępuje blok. Enter zatwierdza i idzie dalej, F8 zatwierdza i zostaje, PgUp/PgDown przechodzą między blokami, Insert wstawia oryginał, Home i End odtwarzają wideo i audio, a Shift+Enter wstawia `\N`. Klawisze można zmienić w edytorze skrótów. Linijka wybrana gdzie indziej albo zmieniona poza asystentem zaczyna się od pierwszego bloku;
* okno koloru Aegisuba (`colorpicker.*`) dla tagów koloru, stylów, dialogów Lua i atrapy wideo. Ma widmo w trybach RGB/R, G, B, HSL/L i HSV/H, pola RGB/HSL/HSV, ASS i HTML, alfę, 32 ostatnie kolory i pipetę: przeciągnięcie albo dwa kliknięcia, powiększenie 7×7, a puszczenie bierze piksel. W polu tekstu kolor jest podglądany na żywo w osobnym kroku undo, który Anuluj cofa;
* motywy kolorów (`theme.*`, strona „Kolory” w ustawieniach). Każdy kolor rysowany przez program (siatka, pole tekstu ze stylami Aegisuba i ich pogrubieniem, audio, narzędzia wizualne) to rola. Są wbudowane motywy Ciemny i Jasny (domyślne kolory Aegisuba), motywy użytkownika w plikach `.ini` (zapisz jako, usuń, przywróć) i paleta okien (systemowa, jasna, ciemna). Zmiana jest widoczna od razu, a Anuluj przywraca poprzedni motyw;
* obroty 3D w narzędziach wizualnych tak, jak liczy je libass (`fs-bridge::visual::projection`): pochylenie `\fax`/`\fay` (skalowane przez `\fscx`/`\fscy`) od punktu (0,0) rysunku, obrót Z, X i Y wokół `\org`, a potem rzut perspektywiczny z kamerą w odległości 312,5 · PlayResY / wysokość klatki. Uchwyty rysowania leżą na wyrenderowanym kształcie, a przeciąganie odwraca rzut. Osie skali, okrąg obrotu Z i siatka obrotu X/Y są rysowane w płaszczyźnie linijki. Test porównuje rzut z ramkami libass dla dziewięciu przypadków;
* opcje narzędzi wizualnych z Kainote, zapamiętywane w ustawieniach. Skala: zmiana wszystkich tagów skali i utrzymanie proporcji. Obrót Z: zmiana wszystkich `\frz` i utrzymanie proporcji. Obrót X/Y: zmiana wszystkich `\frx`/`\fry`. Ruch: `\move` z dwóch punktów i czasu wideo. Kotwicą jest czas pierwszego punktu, a nie początek linijki jak w Kainote. Każda linijka idzie od miejsca, w którym była w tym czasie, w swoich czasach `\move`;
* pozycja i skala według prostokąta (Kainote). Linijka jest mierzona tak, jak rysuje ją libass (`fs-media::bounds`): w skrypcie 3× większym, z linijką przesuniętą o rozmiar klatki, więc część poza ekranem też się liczy, a wynik to ramka całości i samego tekstu (bez obwódki i cienia). Pozycja ustawia linijkę w prostokącie na jeden z 21 sposobów (9 wyrównań, pod, nad, przed i za), skala wypełnia go tekstem na wybranych osiach, a obwódka zostaje bez zmian. Drugi, własny prostokąt (prawy przycisk) zastępuje zmierzony;
* Hydra (narzędzie „wszystkie tagi”): lista tagów użytkownika z zakresem, krokiem, liczbą cyfr, wartościami domyślnymi, miejscem wstawiania i trybem. Suwaki nad wideo pokazują wartości aktywnej linijki, kółko zmienia wartość o krok, Shift+kółko wybiera tag, a prawy przycisk przesuwa suwaki. Dla `\fad` i `\t` klawisze odległości od początku i końca linijki (Ctrl+, i Ctrl+.) biorą czas wideo;
* strona „Skojarzenia plików” w ustawieniach (Windows): ProgID `FSEditor.<rozszerzenie>` w `HKCU\Software\Classes`, więc nie trzeba administratora. Typ dostaje go jako domyślny i na liście „Otwórz za pomocą”, a wyłączenie przywraca poprzedni program. Wybór dokonany w Eksploratorze (UserChoice) ma pierwszeństwo, bo tylko Eksplorator może go zapisać;
* menu kontekstowe pola tekstu: podpowiedzi pisowni, dodanie słowa do słownika i usunięcie go stamtąd, język sprawdzania pisowni (albo jego wyłączenie) i tezaurus;
* autozapis co N sekund (Aegisub): kopia dokumentu w O(1) w GUI, a zapis ASS na dysk w wątku roboczym. Przy awarii i wylogowaniu zapis odbywa się synchronicznie. Obok kopii leży plik `.from` ze ścieżką oryginału, więc „Zapisz” po odzyskaniu zaczyna w tym miejscu;
* przeciąganie w gridzie i w audio poza krawędź przewija dalej na timerze, także gdy mysz stoi, tym szybciej, im dalej jest. Linijki zabierane przez Ctrl+przeciąganie znacznika rysują się razem z nim;
* czcionki dołączone do wideo dla prostokątów pozycji i skali wczytują się w tle, gdy tylko wybierzesz narzędzie. Menedżer stylów nie oznacza jako brakujących czcionek dołączonych do pliku. Czcionka usunięta z `[Fonts]` znika też z ekranu: renderer zaczyna od nowa, bo libass umie usunąć tylko wszystkie naraz.

## 8. Etapy

| Etap | Zakres | Kryterium ukończenia |
|---|---|---|
| 0 | Plan, struktura repo, licencja, toolchain | ten dokument, `cargo check` |
| 1 | `fs-core` + `fs-formats` + `fsed` CLI | testy roundtrip bajt-w-bajt na prawdziwych plikach, testy jednostkowe czasu/VFR/undo/tagów/formatów |
| 2 | `fs-audio` (peaks, FFT, cpal) + `fs-media` (BestSource, libass) | testy na wygenerowanym audio/wideo (seek co do klatki, VFR, audio co do próbki, libass), `fsed media info`, `fsed media frame` |
| 3 | `fs-automation` (Automation 4, karaskel, MoonScript, DependencyControl) | uruchomienie prawdziwych skryptów (kara-templater, skrypty z TypesettingTools) w `fsed lua` |
| 4 | `fs-bridge` + GUI Qt | aplikacja z pełnym układem Kainote, wideo i audio na QRhi |
| 5 | Pakowanie (Windows installer, AppImage, .dmg), CI 3× OS | artefakty z GitHub Actions |
