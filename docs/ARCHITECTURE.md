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
│     └─ src/
│        ├─ lib.rs            # #[cxx::bridge]: typy i funkcje widziane z C++
│        ├─ session/          # jedna karta: dokument z historią undo; każdy plik to `impl Session` jednego obszaru i jego testy
│        │  ├─ mod.rs         # Session: otwieranie, zapis, autozapis, dane projektu, undo/redo, zaznaczenie, mapowanie wierszy
│        │  ├─ rows.rs        # wiersze gridu: odczyt i zapis pól, wstawianie, usuwanie, przenoszenie, sortowanie, schowek, porównanie plików
│        │  ├─ joinsplit.rs   # łączenie, dzielenie i rekombinacja linii (pary tłumaczenia połówkami)
│        │  ├─ trees.rs       # drzewka Kainote
│        │  ├─ timing.rs      # przesuwanie czasów, ciągłość, dociąganie do wideo, postprocesor, zmiana FPS
│        │  ├─ search.rs      # filtr gridu, szukaj/zamień, zamiana zaznaczonych wyników, reguły multireplacera
│        │  ├─ convert.rs     # konwersja formatów (Kainote), eksport (Aegisub), zmiana rozdzielczości
│        │  ├─ styles.rs      # style skryptu, zmiana nazwy idąca za liniami, pliki katalogów
│        │  ├─ attachments.rs # załączniki i kolektor czcionek
│        │  ├─ render.rs      # skrypt dla libass: cały, linie jednej klatki, jedna linia do pomiaru
│        │  ├─ translation.rs # tryb tłumaczenia
│        │  ├─ overrides.rs   # tagi pisane przez narzędzia wizualne, „wszystkie tagi” i Hydrę
│        │  ├─ karaoke.rs     # sylaby paska karaoke
│        │  └─ fixtures.rs    # skrypt i sesja, od których zaczynają testy modułów
│        ├─ automation.rs     # uruchamianie skryptów Lua na sesji, eksport przez filtry
│        ├─ media.rs          # wideo, audio i renderer napisów dla okna
│        ├─ visual.rs         # wartości linii, które edytują narzędzia wizualne (\pos, \move, \clip, \p, skala, obroty)
│        ├─ split.rs          # podziały Kainote zachowujące wygląd (znaki, słowa, klatki \move)
│        └─ kanji.rs, spell.rs  # kanji timer, pisownia i tezaurus
├─ app/                       # GUI Qt 6 Widgets (C++23, CMake + Corrosion)
│  ├─ CMakeLists.txt, CMakePresets.json
│  ├─ src/
│  │  ├─ main.cpp             # start, smoke test (FSED_SMOKE*), raport awarii na Windows
│  │  ├─ bridge.h             # konwersje Qt ↔ cxx (QString ↔ rust::Str/String)
│  │  ├─ mainwindow.h         # MainWindow: jedyny nagłówek okna dla reszty programu; klasa ma kilka jednostek translacji:
│  │  ├─ mainwindow.cpp       #   okno, karty na dole, widok dwóch kart i porównanie, rejestr komend (add), zamykanie
│  │  ├─ mainwindow_menus.cpp #   pasek menu: kolejność budowania, menu Audio, Widok i Pomoc, układ menu Kainote (tabela layOutMenus)
│  │  ├─ mainwindow_filemenu.cpp, _editmenu.cpp, _videomenu.cpp, _subsmenu.cpp  #   komendy menu Plik (z eksportem i sesjami), Edycja, Wideo, Napisy
│  │  ├─ mainwindow_files.cpp #   otwieranie wg rodzaju pliku i parowanie, zapis, konwersja, sesje, ostatnie pliki, zmiany na dysku, autozapis
│  │  ├─ mainwindow_hotkeys.cpp  #   skróty komend, okno skrótów, filtr zdarzeń oddający panelom ich klawisze
│  │  ├─ mainwindow_bars.cpp  #   pasek narzędzi i pasek stanu
│  │  ├─ mainwindow_automation.cpp  #   autoload, menu Automatyzacja i menedżer skryptów, uruchamianie makr
│  │  ├─ mainwindow_corrector.cpp   #   korektor drobnych błędów (reguły multireplacera)
│  │  ├─ mainwindow_p.h       #   wspólne tylko dla tych jednostek: struct Menus, rozszerzenia plików, domyślny pasek, barwy ikon
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
│  │  ├─ icons.*              # ikony SVG z zasobów rysowane w żądanym rozmiarze: barwione ikony paska (icon), ikona programu (appIcon)
│  │  ├─ audiobox.*           # AudioView (QRhiWidget): waveform/spektrogram, karaoke, przyciski
│  │  ├─ rhiquad.*            # jeden teksturowany prostokąt QRhi używany przez wideo i audio
│  │  ├─ findreplace.*        # FindDialog: szukaj/zamień, „znajdź wszystko”, szukanie i zamiana w plikach, okno wyników
│  │  ├─ selectlines.*        # SelectLinesDialog: „Zaznacz linie” Aegisuba
│  │  ├─ styleeditor.*        # StyleEditor: edytor stylu z podglądem, katalogi czcionek Kainote
│  │  ├─ stylemanager.*       # StyleManager: style pliku obok stylów katalogu, edytor w oknie lub zadokowany
│  │  ├─ propertiesdialog.*   # PropertiesDialog: właściwości skryptu ([Script Info])
│  │  ├─ historydialog.*      # HistoryDialog i nazwy kroków undo (undoStepName)
│  │  ├─ resampledialog.*     # ResampleDialog: zmiana rozdzielczości
│  │  ├─ fontcollector.*      # FontCollectorDialog: sprawdzenie, kopiowanie, pakowanie do zip, załączanie czcionek
│  │  ├─ settingsdialog.*     # SettingsDialog: ustawienia, motywy, strony konwersji i domyślnych właściwości skryptu
│  │  ├─ autosavedialog.*     # AutosaveDialog: „Otwórz plik autozapisu”
│  │  ├─ spellcheckdialog.*   # SpellCheckDialog: okno sprawdzania pisowni
│  │  ├─ attachmentsdialog.*  # AttachmentsDialog: załączniki
│  │  ├─ kanjitimer.*         # KanjiTimerDialog: kanji timer Aegisuba
│  │  ├─ assistants.*         # TranslationAssistant i StylingAssistant
│  │  ├─ pasteover.*          # chooseFields: pola dla „Wklej na linie” i kopiowania kolumn
│  │  ├─ tagbuttondialog.*    # TagButtonDialog: ustawienie przycisku tagu editboxa
│  │  ├─ dialogutil.*         # wspólne dla okien i reszty programu: folder autozapisu, katalogi stylów, kolor ASS, ustawienia mediów, cache indeksów
│  │  └─ automationhost.*     # dialogi aegisub.dialog, wybór plików, postęp
│  ├─ icons/                  # ikony paska narzędzi i menu (Tabler Icons, SVG barwione w icons.cpp), zasoby Qt
│  ├─ appicon/                # ikona programu, własna: fsed.svg i fsed-small.svg (poniżej 32 px) jako zasoby Qt dla okien;
│  │                          #   tools/appicon.py robi z nich fsed.ico (zasób exe przez fsed.rc) i fsed.icns (pakiet macOS)
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
| WebVTT | ✔ | ✔ | bloki `NOTE`/`STYLE`/`REGION` pomijane; `<i> <b> <u>` ↔ override, encje, `line`/`align` ↔ `\an` |
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
  * `VideoSource`: BestSource `BestVideoSource`, z klatką RGBA dla dokładnego numeru klatki, tablicą timestampów (VFR), keyframes i rozdzielczością/SAR. Plik Matroska lub MP4 bez indeksu otwiera się z samych pakietów, jak FFMS2 w Aegisubie; BestSource indeksuje go w tle na połowie rdzeni i przejmuje klatki, gdy jego indeks ma te same. Do tego czasu ostatnio zdekodowane klatki zostają w pamięci (256 MB, tyle co cache BestSource), więc krok wstecz nie dekoduje wszystkiego od klatki kluczowej. Klatka wychodzi na dwa sposoby. `frame_rgba` konwertuje ją w swscale na CPU (bicubic, pełna interpolacja chromy w poziomie) macierzą i zakresem z nagłówka `YCbCr Matrix` skryptu, jak w Aegisubie (None: te z wideo, brak nagłówka: TV.601, wideo bez oznaczenia: zgadywane), bez konwersji gamutu i krzywej. Z niej jest zapis i kopia klatki, `blend` libass, pomiary narzędzi wizualnych i `fsed media frame`. `frame_planes` oddaje same płaszczyzny dekodera (planarne YUV 4:2:0, 4:2:2 i 4:4:4, od 8 do 16 bitów) i ich opis (`FramePlanes`): rozmiary, bajty na próbkę, macierz 3×4 na RGB z tych samych współczynników, których używa swscale (`sws_getCoefficients`, skalowanie zakresu, `yuvj*` jako pełny zakres), położenie pierwszej próbki chromy i sufit 15-bitowego wiersza swscale. Odtwarzanie bierze płaszczyzny, a RGB robi z nich karta graficzna (niżej, przy GUI). Klatka bez takich płaszczyzn idzie ścieżką RGBA: format spakowany, RGB, z alfą lub paletą, przeplot, macierz spoza obsługiwanych, a także klatka z metadanymi mastering display (HDR10), której kolory swscale mapuje po swojemu i która ma wyglądać jak dotąd. Pomiar (`decode_speed`, 4K HEVC 10 bitów, 60 kl./s, 61 Mbit/s): 14,3 ms na klatkę do RGBA, 6,3 ms do płaszczyzn (70 i 160 klatek na sekundę).
  * `AudioSource`: `BestAudioSource`, z próbkami PCM `f32` dla zakresu próbek i gotowym mono `i16` wprost z ramek dekodera (`fill_mono`), każda ramka dekodowana raz. Cache ramek BestSource to lista przeszukiwana od początku przy każdej ramce, więc ma tu rozmiar dwóch najdłuższych ramek ścieżki, a nie stały: przy 40-próbkowych ramkach TrueHD cache 16 MB trzymał ich 40 000 i dziesięć minut dekodowało się dwie. `seeks()` mówi, czy BestSource umie przeskakiwać w ścieżce (w AC-3 i DTS-HD MA nie umie i każdy skok dekoduje od początku pliku), a `canceller()` przerywa odczyt z innego wątku. Plik z kilkoma ścieżkami audio wczytuje pierwszą pasującą do preferowanych języków lub nazw z ustawień (Kainote, `fs-bridge::media::preferred_track`), a bez dopasowania pyta, którą wczytać, jak Aegisub.
  * skrypty Avisynth (`.avs`) i VapourSynth (`.vpy`) jako wideo i audio (`cpp/script.cpp`), za tymi samymi `VideoSource` i `AudioSource`, więc okno i `fsed media` otwierają je jak plik. Żadna z bibliotek nie jest linkowana: każda ładuje się przy pierwszym skrypcie swojego rodzaju, a gdy jej nie ma, otwarcie kończy się błędem, który mówi, co zainstalować.
    * Avisynth: `avisynth.dll` z folderu programu albo systemowego (tam, gdzie szuka go Aegisub), poza Windows `libavisynth`, przez interfejs C (`avs_invoke` i pozostałe funkcje szukane po nazwie; struktury i stałe zadeklarowane u nas, bez nagłówka Avisynth). Skrypt wczytuje `Import`, jak w Aegisubie. Każde źródło ma własne środowisko, a wywołania idą pojedynczo, pod jednym muteksem.
    * VapourSynth: biblioteka VSScript wskazana w ustawieniach, a bez tego ta z `VSSCRIPT_PATH`, z rejestru (`SOFTWARE\VapourSynth`, `VSScriptDLL`), ze ścieżki bibliotek i na końcu ta, którą poda `python -m vapoursynth get-vsscript` (instalacja z pip bez rejestracji; wpisy po starej instalacji, które wskazują nieistniejący plik, są pomijane). Nagłówki API (`cpp/vapoursynth/`, LGPL) opisują tylko struktury wskaźników, które VSScript oddaje. Wideo to pierwszy węzeł wideo wśród wyjść skryptu, audio pierwszy węzeł audio.
    * Klatka zostaje w formacie klipu: jej płaszczyzny trafiają do `AVFrame` i przez ten sam `convert` co klatki z pliku, więc macierz, zakres i nagłówek `YCbCr Matrix` działają tak samo. Kolor klatki pochodzi z jej właściwości (`_Matrix`, `_ColorRange`, `_Transfer`, `_Primaries`, `_SARNum`/`_SARDen`; AviSynth+ ma te same nazwy), a bez nich macierz jest zgadywana z rozmiaru, jak w Aegisubie. Formatu, którego swscale nie czyta (YUV float, half float), rdzeń skryptu zamienia na 16 bitów tej samej rodziny, bez zmiany macierzy. Tylko Avisynth starszy niż 2.6 (opcja z Aegisuba, domyślnie wyłączona) sam robi RGB32 przez `ConvertToRGB32` z macierzą, o którą prosi edytor.
    * Liczba klatek i FPS są z klipu, znaczniki czasu stałe. Skrypt VapourSynth może podać własne w `__aegi_timecodes` i klatki kluczowe w `__aegi_keyframes` (tablice liczb, jak w Aegisubie z VapourSynth); klip bez FPS bierze czas trwania pierwszej klatki. Avisynth klatek kluczowych nie podaje.
    * Audio Avisynth idzie drogą Aegisuba: down-mixer z ustawień, `ConvertAudioTo16bit`, potem `ResampleAudio` do wymuszonej częstotliwości albo do 44100 Hz, gdy skrypt ma mniej niż 32000. Audio VapourSynth to próbki 16, 24, 32 bit albo float z klatek po 3072 próbki. Skrypt jest ewaluowany raz na źródło, więc ścieżkę dekoduje jeden dekoder, bez podziału na części. `list_tracks` podaje ścieżkę wideo i audio według tego, co skrypt oddaje, z ostatniej ewaluacji pliku w tej wersji.
  * `SubtitleRenderer`: libass (`ass_read_memory` z aktualnego dokumentu), `render(time) → Vec<AssImage>`. Dla widoku rysuje same napisy jako RGBA z premultiplikowaną alfą, przycięte do prostokąta, który zajmują (`overlay_box`), i odpowiada „to samo co poprzednio”, gdy libass nie widzi zmiany, więc napisy stojące w miejscu nic nie kosztują; na klatkę nakłada je karta graficzna. `blend` nakłada je na klatkę RGBA na CPU (klatka zapisywana z napisami). GUI przekazuje wątkowi renderu kopię dokumentu w O(1) (`RenderSubs`). Zapis skryptu dla libass i rejestracja czcionek z `[Fonts]` odbywają się w tym wątku, nie w GUI.
  * napisy zaszyte w pliku (`extract_subtitles`): Matroska ma indeks bloków (Cues), a FFmpeg i mkvmerge wpisują tam każdy blok ścieżki napisów razem z jego miejscem w klastrze, więc ścieżkę czyta się skokami, kilka bajtów na linijkę (`MatroskaIndex`: 0,6 s zamiast 21 s dla 124 kB napisów w filmie 23 GB). Indeksowi ufa się tylko wtedy, gdy wiadomo, że jest pełny (muxer Lavf albo tag `NUMBER_OF_FRAMES` mkvmerge równy liczbie wpisów), a każdy blok musi być tam, gdzie indeks mówi, należeć do ścieżki i mieć czas z indeksu; FFmpeg liczy miejsce bloku od identyfikatora klastra, a nie od jego zawartości, więc pierwszy blok rozstrzyga, która z dwóch konwencji obowiązuje. Wszystko inne (brak indeksu, kompresja, lacing, WebVTT, inny kontener) czyta się pakietami przez FFmpeg, czyli cały plik: postęp i anulowanie idą wtedy z samych odczytów pliku (`interrupt_callback`), bo FFmpeg odrzuca pozostałe ścieżki u siebie i oddaje sterowanie dopiero przy następnym napisie. Okno robi jedno i drugie poza swoim wątkiem (`read_embedded`), z oknem postępu po 300 ms, jak Aegisub przy Matrosce.
* **`fs-audio` (Rust)**
  * cache szczytów min/max per piksel w kilku poziomach zoomu (waveform). Sumy |x| poziomu są dzielone przez 4^poziom, więc mieszczą się w `u32` na każdym poziomie. Dekodowane PCM trafia od razu do współdzielonego bufora, bez drugiej kopii ścieżki. Audio jest dostępne zaraz po odczytaniu indeksu i dekoduje się w tle, jak cache RAM w Aegisubie (`PcmBuffer::load_in_background`): bufor ma od razu pełną długość, `ready()` mówi, ile próbek od początku jest zdekodowanych, a reszta to cisza. Szczyty dochodzą przyrostowo (`PeakCache::refresh`), kolumny widma sięgające w niezdekodowaną część nie trafiają do cache, a okno pyta o postęp co 100 ms i odrysowuje widok, gdy doszło coś, co pokazuje. Ścieżka dłuższa niż minuta dekoduje się w częściach (do czterech): każda dalsza część ma swój dekoder, który najpierw dekoduje jej początek na boku i bierze część dopiero wtedy, gdy doszedł tam skokiem w dwie sekundy. Dekoder, który nie umie przeskoczyć, jest przerywany, a jego część dekoduje poprzedni, po kolei. Puste audio (2h30m jak w Aegisub) ma 1 kHz: 18 MB zamiast 800;
  * spektrogram: STFT przez `realfft` (okno Hanna, rozmiar 2^n zależny od zoomu), wynik w dB jako tekstury kolumnowe. Próbkowane kolumny leżą na wielokrotnościach jednego kroku dla całego widoku, więc przewinięty widok czyta kolumny z cache. Brakujące liczą się na wszystkich rdzeniach, a cache (64 MB) usuwa najdawniej czytane;
  * odtwarzanie: `cpal` (WASAPI, ALSA/Pulse/PipeWire, CoreAudio), ring buffer, pozycja playheadu raportowana do GUI, resampling do częstotliwości urządzenia; urządzenie wyjścia można wybrać w ustawieniach (brakujące wraca do domyślnego). Opóźnienie audio (Kainote) przesuwa próbki raz przy wczytaniu (bufor zaczyna się o tyle wcześniej lub później niż ścieżka): cisza na początku albo obcięty początek, więc fala, widmo i odtwarzanie zgadzają się bez dalszych przeliczeń.
* **GUI**: `VideoWidget` i `AudioWidget` dziedziczą po `QRhiWidget`. Działa to na D3D11/D3D12, Vulkan i Metal, a OpenGL jest fallbackiem.
  * Widok wideo składa klatkę z napisami w teksturze o rozmiarze klatki i tylko wtedy, gdy zmieni się klatka albo napisy; narzędzia, zoom i przesuwanie rysują tę samą teksturę. Płaszczyzny (tekstury R8 lub R16, bufory z puli, wgrywane bez kopii) przechodzą dwa kroki, w tej samej kolejności co w swscale: `chroma.frag` rozciąga wiersze chromy do szerokości lumy (bicubic swscale: B = 0, C = 0,6, próbka brzegowa powtórzona, wiersz obcięty tak jak 15-bitowy wiersz swscale) do tekstury RGBA16F, a `yuv.frag` interpoluje je w pionie, dokłada lumę i mnoży przez macierz z `FramePlanes`. Klatka, która przyszła jako RGBA, jest do tej tekstury przepisywana. Na to idą napisy. Dopiero całość jest skalowana do widoku filtrem liniowym, tak jak wcześniej klatka z CPU, więc obraz zgadza się z poprzednim przy każdym powiększeniu: test `planes_shade_to_the_frame_swscale_makes` trzyma tę arytmetykę przy klatce swscale (szarości bez różnicy, kolor do 1 poziomu; 601 i 709, zakres ograniczony i pełny, 8 i 10 bitów), a zrzuty okna sprzed zmiany i po niej różnią się najwyżej o 2 poziomy. Jedno odstępstwo: przy chromie wyrównanej do środka swscale gubi jedną próbkę filtra w wierszu i kolumnie 2 lumy, a shader tego nie powtarza (tam do 6 poziomów).
  * Bez tekstur R16 klatki głębsze niż 8 bitów, a bez renderowalnej RGBA16F wszystkie, przychodzą jako RGBA. `video/planes=false` w ustawieniach wymusza RGBA (do porównań).
  * Tekstur tworzonych w `initialize` nie wypełnia się tam obrazem: wgranie czekałoby w partii na pierwszy `render`, który przy gotowej już klatce podmienia teksturę na inną, i trafiłoby do usuniętej.

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
* Kainote w polu edycji: dwuklik na słowie zaznacza tłem pozostałe jego wystąpienia w linijce (całe słowo, bez względu na wielkość liter, także zaraz po `\N` i `\h`; kolor w motywie), dopóki to słowo jest zaznaczone; lista tagów podpowiadanych po `\` mówi przy każdym, co robi (`TagListDelegate`);
* edytor stylu w oknie menedżera stylów (Kainote, pole wyboru „Edytuj style w tym oknie”, domyślnie osobne okno jak w Aegisubie): stoi obok list i zostaje otwarty przy wybieraniu kolejnych stylów; edytowany styl i pozostałe zaznaczone są przy każdym „Zastosuj” szukane po nazwie (`fileApply` / `storageApply`), więc zmiana list pod otwartym edytorem niczego nie myli, a zmiana karty lub katalogu go zamyka;
* sprawdzanie aktualizacji (Aegisub, Kainote; `updates.*`): Pomoc → „Sprawdź aktualizacje” pyta GitHuba o najnowsze wydanie i porównuje numery wersji; nowszą proponuje z opisem wydania i przyciskami strony pobierania, pominięcia tej wersji i przypomnienia. Sprawdzanie przy starcie (raz dziennie) jest wyłączone, dopóki użytkownik nie włączy go w ustawieniach, bo to jedyne miejsce, w którym program sam łączy się z siecią. Otwierana jest tylko strona wydań projektu, cokolwiek przyszło w odpowiedzi;
* Kainote: Shift+klik na przyciskach audio, pola edycji i wideo przypisuje skrót komendy przycisku, a podpowiedź pokazuje jej klawisz. Multireplacer (Popraw drobne błędy) jest niemodalny: reguły mają „tylko w tagach” i wielkość liter (jak w znalezionym, jak wpisano, małe, wielkie), linie to wszystkie, zaznaczone, od zaznaczonej albo ze stylów; Znajdź błędy (w karcie lub we wszystkich) daje listę wyników do odznaczenia i zamiany w miejscu (rdzeń: `replace_rule_matches`, jeden krok cofania na kartę; każdy wynik zamieniany tak, jak go znaleziono w tekście linii, wynik nachodzący na inny już zamieniony jest pomijany, a linie ukryte filtrem siatki nie wchodzą do wyszukiwania), a Zamień działa też we wszystkich kartach. Menu wideo ma strumienie audio (przy kilku ścieżkach, z zaznaczoną wczytaną; wybór przeładowuje audio) i napisów z pliku (wczytywane do karty po pytaniu o zapis, z zaznaczoną ścieżką w karcie, póki nie zastąpi jej inny plik). Nazwy z danych (pliki, style, ścieżki, rozdziały, makra, karty) zachowują „&” w menu i na kartach (`fs::menuText`/`fs::plainText`), a pytanie o zapis bez odpowiedzi niczego nie zapisuje. Opcje: liczenie spacji i interpunkcji w łamaniach, aktywna linia po Ctrl+klik, łączenie „n” w auto-podziale karaoke, przesuwanie granicy sylaby kliknięciem, początkowe powiększenie wideo, czcionka programu, pasek stanu pola tekstu w menu, filtr słów w autozapisach, usuwanie wielu słów słownika, porównanie według wybranych stylów, połączone rozdzielczości we właściwościach, okno usuwania plików tymczasowych (wybrane, wszystkie, starsze od daty), a skrót „ukryj oryginał” bierze oryginał linii w nawiasy klamrowe;
* Kainote: podział linii na znaki, słowa i łamania daje każdemu kawałkowi pełne czasy linii i własny \pos albo \move w miejscu, gdzie był (z rozmiarów tekstu w stylu po tagach, \an linii, tagi sprzed kawałka zachowane, \org przy obrocie); podział na klatki zamienia \move na \pos z danej klatki. Do przypisania: przycisk czcionki, wklejanie całości i zaznaczenia do tłumaczenia, ukrycie oryginału, przyciski tagów 1–20, Stop, pasek postępu, proporcje i usunięcie wideo, usunięcie nieużywanych stylów. Edytor stylu ma filtr czcionek po znakach (domyślnie polskie litery, do zmiany w ustawieniach);
* Kainote: otwarcie wideo proponuje napisy o tej samej nazwie (do bieżącej karty, z pytaniem o zapis), a otwarcie napisów bez wideo, które nie wskazują żadnego, proponuje wideo o tej samej nazwie z folderu; pliki otwierane razem paruje dalej openFiles bez pytań. „Otwórz wideo” z menu idzie tą samą drogą. W ustawieniach wideo są przesunięcia (ms) czasów wstawianych z wideo, nigdy przed zerem;
* Aegisub: przy włączonej pozycji wideo audio cieniuje zakres poprzedniej i bieżącej klatki (kolory w motywie). „Pokaż oryginał” w polu edycji pokazuje tekst linii z chwili jej zaznaczenia (tylko do odczytu), z przyciskami Przywróć, Wyczyść, Wyczyść tekst (zostawia tagi, komentarze i rysunki) i Wstaw oryginał; dwie nowe komendy dostają skróty. Asystent stylów ma własne klawisze do przypisania (Enter zatwierdza, F8 podgląd, PgUp/PgDown linie, Home wideo, End audio) i ramkę „Klawisze”, otwiera się raz. Krzyżyk z Shiftem podaje współrzędne od prawej i dolnej krawędzi, z trzema miejscami po przecinku, gdy wideo jest większe niż rozdzielczość skryptu; dwuklik przenosi tam aktywną linię, a pozostałe zaznaczone o tyle samo (\pos, \move, \org). Na pełnym ekranie dwuklik dalej z niego wychodzi, a bez linii przełącza pełny ekran;
* Aegisub: prawy klik na pasku sylab zmienia tag karaoke całej linijki na \k, \kf albo \ko (jeden krok cofania; obecny jest zaznaczony). Cofnij i Ponów nazywają krok, który wezmą („Cofnij: Edycja linii”, „Nic do cofnięcia”), a nazwy kroków są tłumaczone także w Historii. Komendy, które nie mają na czym działać, są wyszarzone, jak w Aegisubie: zamknięcie i zapis fragmentu audio, zapis i zamknięcie klatek kluczowych i timecodów, szczegóły wideo, cofnięcie do zapisu. Stan jest sprawdzany przy otwarciu menu i po każdej zmianie;
* próbnik koloru z Kainote: prawy klik na przycisku koloru w polu edycji otwiera małe okno w miejscu myszy (pipeta, lupa ekranu, kod koloru), kolor od razu widać w linijce, Anuluj cofa; ustawienie zamienia kliknięcia z oknem wyboru koloru;
* katalogi czcionek z Kainote: nazwane listy rodzin czcionek w ustawieniach (czcionka może być w kilku); w edytorze stylu lista czcionek zawężona do katalogu (lub do czcionek bez katalogu), „+” dodaje lub zabiera bieżącą czcionkę z katalogu, okno katalogów (dodaj / zmień nazwę / usuń, czcionki z podglądem do zaznaczania, filtr);
* edycja wielu stylów naraz (Kainote): Edytuj przy kilku zaznaczonych stylach — po pytaniu pozostałe dostają to, co zmieniło się w edytowanym (bez nazwy), w pliku jednym krokiem cofania (`style_with_changes` / `change_styles`), w magazynie z zapisem katalogu;
* wyszukiwanie zaznaczonej frazy w sieci z menu pola edycji (Kainote): ling.pl, bab.la, Google, synonim.net;
* pasek narzędzi wizualnych na pełnym ekranie (Kainote): przełącznik „Pokaż pasek narzędzi” w panelu pełnego ekranu, domyślnie włączony przy edycji napisów, zapamiętywany;
* zamiana zaznaczonych wyników wyszukiwania (Kainote): pola wyboru przy dopasowaniach i plikach, Zaznacz / Odznacz wszystko, własny tekst zamiany; każde zaznaczone dopasowanie zamieniane tam, gdzie je znaleziono, jednym krokiem cofania na plik (`replace_matches`), linijki zmienione od wyszukania pomijane; pliki z folderu najpierw kopiowane do ReplaceBackup, potem zapisywane w swoim formacie; po zamianie przycisk czeka na nowe wyszukanie;
* domyślne właściwości skryptu z Kainote (Tytuł, Autor, Tłumaczenie, Korekta, Timing, Edycja): strona ustawień z wartością i zaznaczeniem każdej; przy zapisie do ASS zaznaczone różne od pliku są ustawiane jednym krokiem cofania, po potwierdzeniu (Zmień / Zostaw obecne / Zmień i wyłącz potwierdzenie / Anuluj zapis), gdy włączone;
* sposób wczytywania skryptów autoload z Kainote: przy starcie (domyślnie, jak w Aegisubie) lub w tle, albo przy pierwszym otwarciu menu Automatyzacja (w tle lub od razu); wczytywanie w tle to osobny wątek (`start_autoload` / `finish_autoload`), menu otwarte w trakcie pokazuje wiersz o wczytywaniu i przebudowuje się po jego końcu; menedżer, eksport i skróty makr jeszcze niewczytanych skryptów najpierw kończą wczytywanie;
* sesje w plikach (Kainote'owe zapisywanie i wczytywanie sesji): własny format `.fses` (JSON: pliki zakładek — napisy, a bez nich wideo — i indeks zakładki z przodu), otwierany też z linii poleceń i przeciąganiem; przed zapisem pytanie o niezapisane zmiany, brakujące pliki pomijane przy wczytaniu; bez importu `.kls`;
* limit plików indeksów BestSource (Aegisubowy CleanCache indeksów FFMS2, 20 plików / 42 MB; limit plików cache Kainote): po każdym otwarciu wideo lub audio, w osobnym wątku; indeksy otwartego pliku dostają bieżącą datę, więc znikają najdawniej używane, a nie te w użyciu (`fs_media::clean_cache`);
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
