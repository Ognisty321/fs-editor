# Korpus testowy

| Katalog | Zawartość |
|---|---|
| `aegisub/` | skrypty `.ass` z repozytorium Aegisub (format, karaoke, templater, rysunki) |
| `vfr/` | pliki timecode v1 i v2, poprawne i błędne; wyniki referencyjne z testów Aegisub |
| `keyframe/` | pliki keyframe'ów w formatach Aegisub, XviD i x264, plus pusty i błędny |
| `charset/` | próbki kodowań: ASCII, UTF-8, UTF-8 z BOM, ucięty UTF-8, windows-1250, plik binarny |

Pliki są oznaczone `-text` w `.gitattributes`. Dzięki temu git nie zmienia w nich końców linii, a testy porównują bajty.

Własne pliki `.ass` wrzuć do `tests/corpus-local/` (katalog jest ignorowany przez git) albo wskaż ich katalog zmienną `FS_CORPUS_DIR`. Każdy taki plik przechodzi wtedy przez wszystkie testy korpusowe.
