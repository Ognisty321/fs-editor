# Test corpus

| Directory | Contents |
|---|---|
| `aegisub/` | `.ass` scripts from the Aegisub repository (format, karaoke, templater, drawings) |
| `vfr/` | v1 and v2 timecode files, valid and broken; reference results from the Aegisub tests |
| `keyframe/` | keyframe files in the Aegisub, XviD and x264 formats, plus an empty and a broken one |
| `charset/` | encoding samples: ASCII, UTF-8, UTF-8 with BOM, truncated UTF-8, windows-1250, a binary file |

The files are marked `-text` in `.gitattributes`, so git leaves their line endings alone and the tests can compare bytes.

Put your own `.ass` files in `tests/corpus-local/` (git ignores that directory) or point to their directory with the `FS_CORPUS_DIR` variable. Every such file then goes through all the corpus tests.
