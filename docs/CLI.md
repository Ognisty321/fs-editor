# `fsed`: narzędzie wiersza poleceń

`fsed` pracuje bezpośrednio na rdzeniu FS Editora: tym samym modelu dokumentu i tych samych operacjach, których używa GUI.

```bash
cargo run --release -p fs-cli -- <polecenie> [opcje]
```

## Zasady wspólne

* **Formaty wejścia** są wykrywane po treści i rozszerzeniu: ASS, SSA, SRT, MPL2, MicroDVD, TMP i TTXT (MPEG-4 Timed Text).
  * `--format` wymusza format.
  * `--encoding` wymusza kodowanie, np. `windows-1250`.
  * `--input-fps` podaje FPS dla MicroDVD bez nagłówka; domyślnie 23.976.
* **Kodowanie** jest wykrywane kolejno po BOM, UTF-16, UTF-8 i na koniec przez chardetng z podpowiedzią dla języka polskiego.
* **Wyjście** poleceń, które zmieniają plik, trafia:
  * na stdout (domyślnie);
  * do pliku przez `-o plik`;
  * do pliku wejściowego przez `--in-place`. Zapis jest atomowy: plik tymczasowy, potem zmiana nazwy.

  Format, kodowanie i BOM pliku wejściowego zostają zachowane.
* **Bezstratność**: linie, których polecenie nie zmieniło, są zapisywane z oryginalnego tekstu, razem z końcami linii, komentarzami i nieznanymi sekcjami.
* **Czas** można podać jako:
  * `1.5s`, `-250ms`, `300` (milisekundy);
  * `0:00:02.10`, `+0:01.00`.
* **FPS** podaje się przez `--fps 23.976` albo `--fps 24000/1001`. Wartości 23.976, 29.97, 47.952 i 59.94 są zamieniane na dokładne ułamki NTSC.
* **VFR** obsługuje się przez `--timecodes plik.txt` (formaty v1 i v2).

## Polecenia

| Polecenie | Opis |
|---|---|
| `info FILE` | Format, kodowanie, rozdzielczość, sekcje, liczba stylów i linii, długość, CPS, kolizje. |
| `check FILE…` | Ostrzeżenia parsera, brakujące style, koniec przed startem, CPS i kolizje w tym samym stylu i warstwie. Kod wyjścia 1 przy błędach. Z `--strict` CPS i kolizje też są błędami. |
| `roundtrip FILE…` | Sprawdza, czy wczytanie i zapis dają identyczne bajty. |
| `lines FILE` | Tabela linii jak w gridzie. Przyjmuje te same filtry co `filter`; `--strip` usuwa tagi z wyświetlanego tekstu. |
| `styles FILE` | Lista stylów. |
| `fonts FILE` | Kolekcjoner czcionek jak w Aegisubie: kroje użyte w liniach (z `\fn`, `\b`, `\i`, `\r`), plik z czcionką, brakujące glify i sztuczne pogrubienie/kursywa (`FAUX`). `--dir KATALOG` przeszukuje dodatkowy folder, `--collect KATALOG` kopiuje znalezione pliki. Kod wyjścia 1, gdy czegoś brakuje. |
| `convert IN -o OUT` | Konwersja. Format wyjścia bierze się z `--to` albo z rozszerzenia. Opcje: `--out-encoding`, `--no-bom`, `--output-fps` (MicroDVD). |
| `shift FILE` | Przesuwa czasy o `--by CZAS` albo o `--frames N`; ta druga opcja wymaga `--fps` lub `--timecodes`. |
| `sort FILE --by KLUCZ` | Sortowanie po `start`, `end`, `style`, `actor`, `effect`, `layer` albo `text`. |
| `filter FILE` | Zostawia tylko pasujące linie, resztę usuwa. |
| `replace FILE --find X --replace Y` | Znajdź i zamień. |
| `resample FILE --to 1920x1080` | Zmienia rozdzielczość skryptu: style, `\pos`, `\move`, `\org`, `\clip`, rysunki, `\fs`, `\bord` itd. Domyślnie rozdzielczość źródłowa pochodzi z PlayRes. `--stretch` rozciąga obraz przy zmianie proporcji. |
| `change-fps FILE --from A --to B` | Przelicza czasy między stałymi FPS. |
| `postprocess FILE` | Postprocesor timingu w stylu Aegisub. |
| `tl status`, `tl enable`, `tl disable` | Tryb tłumaczenia zgodny z Kainote: postęp, włączenie i wyłączenie. `disable` zostawia tłumaczenia. |
| `tl paste FILE TXT` | Wkleja tłumaczenia z pliku tekstowego, jedna linia na napis. `--from-row` wskazuje pierwszy wiersz. |
| `tl extract-original FILE` | Wypisuje oryginały, jeden na linię, do tłumaczenia poza programem. |
| `frame WARTOŚĆ` | Zamienia czas na klatkę albo odwrotnie. Numer klatki poprzedza się literą `f`, np. `f1500`. Wypisuje czasy exact, start i end oraz SMPTE. |
| `timecodes IN -o OUT` | Konwertuje timecody v1 lub v2 do v2. `--frames N` przedłuża plik o ekstrapolowane klatki. |
| `lua SKRYPT [FILE]` | Automatyzacja zgodna z Aegisub Automation 4 (Lua i MoonScript). Bez pliku wypisuje makra i filtry eksportu skryptu. Z plikiem uruchamia `--macro NAZWA` albo `--filter NAZWA` (nazwa lub numer z listy) i zapisuje wynik. |

Opcje poleceń, które wymagają rozwinięcia:

* **`shift`**:
  * `--start-only` / `--end-only` przesuwają tylko jeden z czasów;
  * `--from-line N` ogranicza zmianę do linii od numeru N;
  * `--after CZAS` ogranicza ją do linii startujących od podanego czasu;
  * `--styles A,B` ogranicza ją do podanych stylów.
* **`filter`**:
  * `--style`, `--actor` i `--effect` filtrują po polach;
  * `--text REGEX` dopasowuje treść;
  * `--comments` i `--dialogue` wybierają rodzaj linii;
  * `--overlapping` zostawia linie nakładające się na inne;
  * `--min-cps N` zostawia linie powyżej N znaków na sekundę;
  * `--invert` odwraca wynik.
* **`replace`**:
  * `--regex` włącza wyrażenia regularne;
  * `-i` ignoruje wielkość liter;
  * `--skip-tags` nie zamienia niczego wewnątrz `{…}`;
  * `--field text|style|actor|effect` wybiera pole.
* **`postprocess`**:
  * `--lead-in` i `--lead-out` dodają zapas przed i po linii;
  * `--adjacent CZAS` łączy sąsiednie linie z przerwą mniejszą niż CZAS;
  * `--bias 0..1` ustala punkt połączenia;
  * `--keyframes` z `--fps` lub `--timecodes` i `--snap` przyciąga czasy do keyframe'ów.

* **`lua`**:
  * `--rows 1,4-7` zaznacza wiersze (domyślnie wszystkie), `--active N` ustawia aktywny wiersz;
  * makro uruchamia się tylko, gdy jego `validate` zwróci prawdę;
  * filtr dostaje domyślne wartości ze swojego panelu konfiguracji, a okna dialogowe makr są zatwierdzane domyślnym przyciskiem;
  * `--fps` lub `--timecodes` dają skryptowi `aegisub.frame_from_ms` i `ms_from_frame`;
  * `--user KATALOG` wskazuje katalog z `automation/{autoload,include}` użytkownika (domyślnie `%APPDATA%\fs-editor` albo `~/.config/fs-editor`), `--data KATALOG` katalog z dołączonym `automation/`;
  * `--trace-level 0..5` ustala próg `aegisub.debug.out`; komunikaty skryptu trafiają na stderr.
  * skrypty z DependencyControl zachowują się jak w Aegisubie: brakujące moduły z zaufanych feedów DepCtrl doinstalowuje sam do katalogu `--user` (także natywne biblioteki, np. SubInspector), a feedy zapisuje w katalogu tymczasowym. Do eksperymentów warto podać osobny `--user`.

## Przykłady

Opóźnienie napisów o 1,2 s od 10. minuty:

```bash
fsed shift odc01.ass --by 1.2s --after 0:10:00.00 --in-place
```

Przesunięcie o 3 klatki przy wideo VFR:

```bash
fsed shift odc01.ass --frames 3 --timecodes tc.txt -o out.ass
```

Konwersja do SRT w CP1250:

```bash
fsed convert odc01.ass -o odc01.srt --out-encoding windows-1250 --no-bom
```

Linie powyżej 20 CPS bez tagów:

```bash
fsed lines odc01.ass --dialogue --min-cps 20 --strip
```

Tłumaczenie poza programem:

```bash
fsed tl extract-original en.ass > en.txt
```

```bash
fsed tl paste en.ass pl.txt -o pl.ass
```

Szablony karaoke (kara-templater) na całym pliku:

```bash
fsed lua crates/fs-automation/automation/autoload/kara-templater.lua kara.ass --macro "Apply karaoke template" -o kara-fx.ass
```

## Polecenia `media`

Polecenia `media` wymagają budowy z funkcją `media`: `cargo build -p fs-cli --features media` (zależności z `tools/deps`). Dekodowanie robi BestSource, a renderowanie napisów libass. Indeks każdego pliku jest zapisywany w `%LOCALAPPDATA%\fs-editor\bsindex` albo `~/.cache/fs-editor/bsindex`, więc kolejne otwarcia są natychmiastowe.

| Polecenie | Opis |
|---|---|
| `media info FILE` | Lista strumieni (z językiem i tytułem) oraz parametry wideo: rozdzielczość, format, macierz kolorów, liczba klatek, FPS, czy plik jest VFR, keyframe'y. Do tego parametry audio i fonty załączone w MKV. |
| `media frame VIDEO --frame N` | Zapisuje klatkę N do PNG (`-o`). Zamiast `--frame` można podać `--time CZAS`, wtedy zapisana zostanie klatka widoczna w tym czasie. `--subs PLIK` nakłada napisy przez libass z fontami z MKV. |
| `media keyframes VIDEO` | Lista keyframe'ów w formacie Aegisuba. |
| `media timecodes VIDEO` | Timecody v2 z rzeczywistych znaczników czasu klatek. |
| `media audio-clip FILE --start A --end B -o clip.wav` | Wycinek audio jako 16-bitowy WAV (odpowiednik „Save audio clip” w Aegisubie). `--mono` miksuje kanały do mono. |
