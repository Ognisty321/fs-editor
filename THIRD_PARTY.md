# Third-party components and licences

FS Editor is licensed under BSD-3-Clause.

* Dependencies with compatible licences (MIT, Apache-2.0, BSD, ISC, Zlib) are linked statically.
* Dependencies under copyleft licences (LGPL: Qt, FFmpeg, FriBidi) are linked **dynamically**, so that users can replace them.
* The FFmpeg `gpl` and `nonfree` features are disabled.

Kainote code is **not** copied. The interface layout is recreated from the program's behaviour and screenshots; the clean-room rules are in [docs/research/kainote-ui.md](docs/research/kainote-ui.md).

## Rust crates (core and CLI)

Versions are pinned in `Cargo.lock`. `cargo metadata` prints the full list, including transitive dependencies. Licences:

| Crate | Licence |
|---|---|
| regex, regex-automata, regex-syntax | MIT OR Apache-2.0 |
| aho-corasick, memchr | Unlicense OR MIT |
| encoding_rs | (Apache-2.0 OR MIT) AND BSD-3-Clause |
| chardetng | Apache-2.0 OR MIT |
| clap (+ clap_builder, clap_derive, clap_lex, anstream, anstyle*) | MIT OR Apache-2.0 |
| strsim | MIT |
| anyhow, thiserror | MIT OR Apache-2.0 |
| tempfile (tests only), fastrand, rustix, getrandom, libc, windows-sys | MIT OR Apache-2.0 (rustix, linux-raw-sys: also Apache-2.0 WITH LLVM-exception) |
| unicode-ident | (MIT OR Apache-2.0) AND Unicode-3.0 |
| proc-macro2, quote, syn, heck | MIT OR Apache-2.0 |
| mlua, mlua-sys, luajit-src | MIT |
| pcre2-sys (PCRE2 built from source) | MIT / Unlicense (crate), BSD-3-Clause (PCRE2) |
| icu_casemap and ICU4X dependencies | Unicode-3.0 |
| fontdb, ttf-parser | MIT OR Apache-2.0 |
| spellbook (spell checking, Hunspell dictionaries) | MPL-2.0 (file-level copyleft; the crate is used unmodified) |
| cc (build only) | MIT OR Apache-2.0 |

## Native, GUI and automation dependencies

Versions come from [docs/research/dependencies.md](docs/research/dependencies.md).

| Component | Licence | How it is included |
|---|---|---|
| Qt 6.12 | LGPLv3 | dynamically |
| `qsb` (qtshadertools) | GPLv3 | build time only, not distributed |
| BestSource R23 | MIT | statically |
| FFmpeg 9.0 | LGPL-2.1+ | dynamically, without `gpl` and `nonfree` |
| xxHash | BSD-2-Clause | statically |
| VapourSynth API headers (`VapourSynth4.h`, `VSScript4.h`, R79) | LGPL-2.1+ | headers only, in `crates/fs-media/cpp/vapoursynth/`; VapourSynth itself is neither linked nor bundled, it is loaded at run time from the user's installation |
| AviSynth (AviSynth+) | GPL-2+ with an exception for the interface | nothing is bundled: the user's `avisynth.dll` is loaded at run time and called through the C interface, with our own declarations |
| libp2p (sekrit-twc) | WTFPL | statically |
| libass 0.17.5 | ISC | statically or dynamically |
| FreeType | FTL | as a libass dependency |
| HarfBuzz, Fontconfig | MIT | — |
| FriBidi | LGPL-2.1+ | dynamically |
| cxx, cpal, realfft | MIT / Apache-2.0 | crates |
| LuaJIT | MIT | through `mlua` (vendored) |
| LPeg 1.1.0 | MIT | C sources built by `cc` |
| luabins 0.3 (from the Aegisub subproject) | MIT | C sources built by `cc`, `crates/fs-automation/vendor/luabins` (licence in `COPYRIGHT`) |
| MoonScript | MIT | bundled `moonscript.lua` file with the upstream licence |
| Aegisub Automation 4 includes and autoloads (karaskel, utils, unicode, re, clipboard, lfs, kara-templater …) | BSD-3 / ISC / MIT | `crates/fs-automation/automation/`, with their headers kept; files without a header are covered by `LICENCE-Aegisub` |
| Kana↔romaji table from libaegisub (`kana_table.cpp`) | ISC, © 2013 Thomas Goyne | data in `crates/fs-core/src/kana_table.rs` (kanji timer) |
| Corrosion | MIT | build tool |
| Tabler Icons 3.49.0 | MIT | SVG files in `app/icons/` (licence in `app/icons/LICENSE`), compiled in as Qt resources |

## Spelling dictionaries

The dictionaries are not in the repository, but the CI packages include them: `tools/dictionaries.py` downloads pinned versions (verified by SHA-256) into the package's `bin/dictionaries`, together with each one's README/licence files. Other Hunspell dictionaries (`.aff` + `.dic`) and MyThes thesauri (`th_*.idx` + `.dat`) can be dropped into that directory, or a folder can be set in the settings.

| Dictionary | Source | Licence |
|---|---|---|
| Polish `pl_PL` | [sjp.pl/sl/en](https://sjp.pl/sl/en/), file `sjp-hunspell-pl-*.zip`; sjp.pl keeps only the latest release, so when the pinned one disappears, a copy of this dictionary from [LibreOffice/dictionaries](https://github.com/LibreOffice/dictionaries/tree/master/pl_PL) is used until a new one is pinned | choice of GPL-2, LGPL-2.1, MPL-1.1, CC BY 4.0, Apache-2.0 |
| English `en_US` | [en-wl/wordlist](https://github.com/en-wl/wordlist/releases), file `hunspell-en_US-*.zip` (SCOWL) | SCOWL licence (MIT-like) |
| Polish thesaurus `th_pl_PL_v2` | Dobry słownik / Marcin Miłkowski, from [LibreOffice/dictionaries](https://github.com/LibreOffice/dictionaries/tree/master/pl_PL) | LGPL-2.1 |
| English thesaurus `th_en_US_v2` | WordNet (Princeton), from [LibreOffice/dictionaries](https://github.com/LibreOffice/dictionaries/tree/master/en) | WordNet licence (BSD-like) |

## Test files

| Directory | Origin | Licence |
|---|---|---|
| `tests/corpus/aegisub/` | sample scripts from the Aegisub repository | Aegisub licence (BSD-3 / ISC) |
| `tests/corpus/vfr/`, `tests/corpus/keyframe/`, `tests/corpus/charset/` | test data from `tests/` in the Aegisub repository | Aegisub licence (BSD-3 / ISC) |
| `crates/fs-automation/tests/aegisub/` | Automation 4 tests and module specs from the Aegisub repository | Aegisub licence (BSD-3 / ISC) |
| `tests/corpus-local/` | the user's private files | not distributed (`.gitignore`) |
