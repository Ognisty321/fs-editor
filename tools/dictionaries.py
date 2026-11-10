"""The spell checking dictionaries and thesauri a package carries: `python tools/dictionaries.py <dir>`.

Downloads the pinned versions below (each checked by its SHA-256), unpacks the Hunspell
`.aff`/`.dic` files, and writes each MyThes thesaurus's `.idx` beside its `.dat`, as LibreOffice
builds them (`th_gen_idx.pl`): the charset line, the entry count, then `word|byte offset` sorted.
A newer version: change its URL and hash here.

sjp.pl keeps only its latest build, under a dated name, so the pinned Polish dictionary is gone
from there once a newer one is out. The copy of it LibreOffice ships, pinned by commit like the
thesauri, then stands in (with a note), and a build still gets a Polish dictionary; pinning the
new sjp.pl file here brings the current one back.
"""

import hashlib
import io
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

LO = "https://raw.githubusercontent.com/LibreOffice/dictionaries/67032ee957631908098a72edf48dc81cac27e369"

# What stands in for the Polish dictionary when sjp.pl no longer has the pinned build.
POLISH_FALLBACK = [
    (LO + "/pl_PL/pl_PL.aff", "82973651651aa930335c865b339b98db376ca3dbf3a661b70b9eeb71fdf41dca",
     {None: "pl_PL.aff"}),
    (LO + "/pl_PL/pl_PL.dic", "c0848440599eb88e5aca500418d5f389e562ec2c157b63dbe39d354658ffba49",
     {None: "pl_PL.dic"}),
    (LO + "/pl_PL/README_pl.txt", "ce3ad7ab1d3a8b767b8f7dcc870796fbda76bc7ad8cde22f6312b0cf86a5bd11",
     {None: "README_pl_PL.txt"}),
]

# url, sha256, {member path in the zip (a nested zip: "outer>inner") or None for the file itself: saved name}
# and, for a source that does not keep its old versions, the sources to use when the URL is gone
SOURCES = [
    ("https://sjp.pl/sl/ort/sjp-hunspell-pl-20261101.zip",
     "a29b40d80a2501ba1ad84fa17f7d177e2ebe313082f74f837a77b3135c05340c",
     {"pl_PL.zip>pl_PL.aff": "pl_PL.aff", "pl_PL.zip>pl_PL.dic": "pl_PL.dic",
      "pl_PL.zip>README_pl_PL.txt": "README_pl_PL.txt"},
     POLISH_FALLBACK),
    ("https://github.com/en-wl/wordlist/releases/download/rel-2026.02.25/hunspell-en_US-2026.02.25.zip",
     "ac8e73310e951d88c52c2cf2ba54ceaca34f8486a81630ac8a75dc5f931179f9",
     {"en_US.aff": "en_US.aff", "en_US.dic": "en_US.dic", "README_en_US.txt": "README_en_US.txt"}),
    (LO + "/pl_PL/th_pl_PL_v2.dat", "8f1e562647c9f3816fb760338ffa17df54bd13ae0150b07dd039b98c4e693bd1",
     {None: "th_pl_PL_v2.dat"}),
    (LO + "/pl_PL/README_pl.txt", "ce3ad7ab1d3a8b767b8f7dcc870796fbda76bc7ad8cde22f6312b0cf86a5bd11",
     {None: "README_th_pl_PL_v2.txt"}),
    (LO + "/en/th_en_US_v2.dat", "8a3e4637450b7277428da248f0a604b5c92942bc30507220c051181462340f39",
     {None: "th_en_US_v2.dat"}),
    (LO + "/en/WordNet_license.txt", "23dbc4f3ea771304e5ab3e5709843813d2619f3c0cfc81229b53fcdb9985f29a",
     {None: "README_th_en_US_v2.txt"}),
]


def fetch(url, sha256):
    """The pinned file, or None when the server no longer has it (404 or 410)."""
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
    except urllib.error.HTTPError as e:
        if e.code in (404, 410):
            return None
        raise
    if hashlib.sha256(data).hexdigest() != sha256:
        sys.exit(f"{url}: SHA-256 mismatch, not the pinned version")
    return data


def install(out, url, sha256, files, fallback=()):
    data = fetch(url, sha256)
    if data is None:
        if not fallback:
            sys.exit(f"{url}: not there any more, pin its current version in tools/dictionaries.py")
        print(f"note: {url} is gone (a newer build replaced it); using the pinned copy from "
              f"{fallback[0][0].rsplit('/', 1)[0]} instead. Pin the new file in tools/dictionaries.py.")
        for source in fallback:
            install(out, *source)
        return
    for path, name in files.items():
        (out / name).write_bytes(data if path is None else member(data, path))
        print(name)


def member(data, path):
    for part in path.split(">"):
        data = zipfile.ZipFile(io.BytesIO(data)).read(part)
    return data


def thesaurus_index(dat):
    """The .idx of a MyThes .dat: each entry's `word|count` line, with the meaning lines after it."""
    entries, skip = {}, 0
    lines = dat.split(b"\n")
    charset = lines[0].strip()
    pos = len(lines[0]) + 1
    for line in lines[1:]:
        if skip:
            skip -= 1
        else:
            word, _, count = line.rstrip(b"\r").rpartition(b"|")
            # a line broken off a meaning (th_pl_PL_v2 has one) is no entry: MyThes' th_gen_idx.pl
            # would index it as a word of no meanings, hiding that word's real entry
            if word and count.isdigit():
                entries.setdefault(word, pos)  # a repeated word: the first entry, as a lookup finds it
                skip = int(count)
        pos += len(line) + 1
    for word, off in entries.items():  # each offset starts that word's entry
        assert dat.startswith(word + b"|", off), word
    return b"".join([charset, b"\n", str(len(entries)).encode(), b"\n"] +
                    [w + b"|" + str(o).encode() + b"\n" for w, o in sorted(entries.items())])


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for source in SOURCES:
        install(out, *source)
    for dat in sorted(out.glob("th_*.dat")):
        dat.with_suffix(".idx").write_bytes(thesaurus_index(dat.read_bytes()))
        print(dat.with_suffix(".idx").name)


if __name__ == "__main__":
    main()
