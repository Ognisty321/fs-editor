# Komponenty zewnętrzne i licencje

FS Editor jest na licencji BSD-3-Clause.

* Zależności ze zgodnymi licencjami (MIT, Apache-2.0, BSD, ISC, Zlib) są linkowane statycznie.
* Zależności na licencjach copyleft (LGPL: Qt, FFmpeg, FriBidi) są linkowane **dynamicznie**, żeby użytkownik mógł je podmienić.
* Funkcje `gpl` i `nonfree` FFmpeg są wyłączone.

Kod Kainote **nie jest** kopiowany. Układ interfejsu jest odtworzony na podstawie zachowania programu i zrzutów ekranu; zasady clean-room są w [docs/research/kainote-ui.md](docs/research/kainote-ui.md).

## Crate'y Rust (rdzeń i CLI)

Wersje są przypięte w `Cargo.lock`. Pełną listę wraz z zależnościami przechodnimi wypisuje `cargo metadata`. Licencje:

| Crate | Licencja |
|---|---|
| regex, regex-automata, regex-syntax | MIT OR Apache-2.0 |
| aho-corasick, memchr | Unlicense OR MIT |
| encoding_rs | (Apache-2.0 OR MIT) AND BSD-3-Clause |
| chardetng | Apache-2.0 OR MIT |
| clap (+ clap_builder, clap_derive, clap_lex, anstream, anstyle*) | MIT OR Apache-2.0 |
| strsim | MIT |
| anyhow, thiserror | MIT OR Apache-2.0 |
| tempfile (tylko testy), fastrand, rustix, getrandom, libc, windows-sys | MIT OR Apache-2.0 (rustix, linux-raw-sys: także Apache-2.0 WITH LLVM-exception) |
| unicode-ident | (MIT OR Apache-2.0) AND Unicode-3.0 |
| proc-macro2, quote, syn, heck | MIT OR Apache-2.0 |
| mlua, mlua-sys, luajit-src | MIT |
| pcre2-sys (PCRE2 budowane ze źródeł) | MIT / Unlicense (crate), BSD-3-Clause (PCRE2) |
| icu_casemap i zależności ICU4X | Unicode-3.0 |
| fontdb, ttf-parser | MIT OR Apache-2.0 |
| spellbook (sprawdzanie pisowni, słowniki Hunspell) | MPL-2.0 (copyleft na poziomie pliku; crate używany bez zmian) |
| cc (tylko budowanie) | MIT OR Apache-2.0 |

## Zależności kolejnych etapów

Wersje pochodzą z [docs/research/dependencies.md](docs/research/dependencies.md).

| Komponent | Licencja | Sposób dołączenia |
|---|---|---|
| Qt 6.12 | LGPLv3 | dynamicznie |
| `qsb` (qtshadertools) | GPLv3 | tylko w czasie budowania, nie jest dystrybuowany |
| BestSource R22 | MIT | statycznie |
| FFmpeg 9.0 | LGPL-2.1+ | dynamicznie, bez `gpl` i `nonfree` |
| xxHash | BSD-2-Clause | statycznie |
| libp2p (sekrit-twc) | WTFPL | statycznie |
| libass 0.17.5 | ISC | statycznie lub dynamicznie |
| FreeType | FTL | wariant FTL |
| HarfBuzz, Fontconfig | MIT | — |
| FriBidi | LGPL-2.1+ | dynamicznie |
| cxx, cpal, realfft, mlua | MIT / Apache-2.0 | crate'y |
| LuaJIT | MIT | przez `mlua` (vendored) |
| LPeg 1.1.0 | MIT | źródła C budowane przez `cc` |
| luabins 0.3 (z podprojektu Aegisub) | MIT | źródła C budowane przez `cc`, `crates/fs-automation/vendor/luabins` (licencja w `COPYRIGHT`) |
| MoonScript | MIT | dołączony plik `moonscript.lua` z licencją upstream |
| Include'y i autoloady Automation 4 Aegisub (karaskel, utils, unicode, re, clipboard, lfs, kara-templater …) | BSD-3 / ISC / MIT | `crates/fs-automation/automation/`, z zachowanymi nagłówkami; pliki bez nagłówka objęte `LICENCE-Aegisub` |
| Tabela kana↔romaji z libaegisub (`kana_table.cpp`) | ISC, © 2013 Thomas Goyne | dane w `crates/fs-core/src/kana_table.rs` (kanji timer) |
| Corrosion | MIT | narzędzie budowania |
| Tabler Icons 3.49.0 | MIT | pliki SVG w `app/icons/` (licencja w `app/icons/LICENSE`), wkompilowane jako zasoby Qt |

## Słowniki pisowni

Słowniki nie są dołączane do repozytorium. Pliki `.aff` i `.dic` (Hunspell) wrzuca się do katalogu `dictionaries` obok programu albo wskazuje w ustawieniach. Sprawdzone źródła:

| Słownik | Źródło | Licencja |
|---|---|---|
| polski `pl_PL` | [sjp.pl/sl/en](https://sjp.pl/sl/en/), plik `sjp-hunspell-pl-*.zip` | do wyboru GPL-2, LGPL-2.1, MPL-1.1, CC BY 4.0, Apache-2.0 |
| angielski `en_US` | [en-wl/wordlist](https://github.com/en-wl/wordlist/releases), plik `hunspell-en_US-*.zip` (SCOWL) | licencja SCOWL (typu MIT) |

## Pliki testowe

| Katalog | Pochodzenie | Licencja |
|---|---|---|
| `tests/corpus/aegisub/` | przykładowe skrypty z repozytorium Aegisub | licencja Aegisub (BSD-3 / ISC) |
| `tests/corpus/vfr/`, `tests/corpus/keyframe/`, `tests/corpus/charset/` | dane testowe z `tests/` repozytorium Aegisub | licencja Aegisub (BSD-3 / ISC) |
| `crates/fs-automation/tests/aegisub/` | testy Automation 4 i specyfikacje modułów z repozytorium Aegisub | licencja Aegisub (BSD-3 / ISC) |
| `tests/corpus-local/` | prywatne pliki użytkownika | nie są dystrybuowane (`.gitignore`) |
