<version> is the bare version, for example 0.1.3. Copy everything below the line into
docs/releases/v<version>.md, fill it in, and delete what you leave unused (this paragraph and the
line too). Rules:

- English only. Plain, concrete sentences: say what changed for the user, then why if it matters.
- The body is shown on GitHub as Markdown and in the program's update dialog as plain text, so the
  summary line and Highlights come first and must make sense without formatting. Downloads and
  Install stay at the end. No emoji, no badges, no HTML.
- Section order is fixed. Drop a section that has nothing in it instead of writing "None".
- Inside New / Improved / Fixed / Under the hood, group related items under a bold lead-in
  (`- **Area.** ...`) or a `###` heading when a change needs several bullets or a table.
- Benchmarks: one table per change, a caption row naming the test material, columns Before / After,
  units in every cell. Say how the result was checked to be the same where that matters.
- Use the program's English UI names for menus, dialogs and settings, in quotes ("Check for updates...").
- Keep the Install section word for word; only the version in the file names changes.
- Every placeholder is written as `<Capitalised text>`; replace them all. tools/release-check.sh refuses
  notes that still contain one, `<version>`, or this paragraph's dashed line.

----------------------------------------------------------------------------------------------------

<Summary: one sentence, what this release is about, readable as plain text.>

## Highlights

- <Highlight: three to five bullets, the changes a user would want to hear about first.>

## New

- **<Feature>.** <What it does and where to find it.>

## Improved

- **<Area>.** <What works better than before.>

| <Test material> | Before | After |
|---|---|---|
| <Operation measured> | <Before, with unit> | <After, with unit> |

## Fixed

- <What went wrong, in which situation, and that it no longer does.>

## Under the hood

- <Build, CI, packaging and code-structure changes that do not change behaviour.>

## Known limitations

- macOS is not built or tested yet.

## Downloads

| File | Platform | Notes |
|---|---|---|
| `FS-Editor-<version>-windows-x64.zip` | Windows 10/11, 64-bit | |
| `FS-Editor-<version>-linux-x64.zip` | Linux x86-64 | built on Ubuntu 24.04 |
| `SHA256SUMS.txt` | | SHA-256 checksums of both packages |

## Install

Unzip the package anywhere and run `bin/fsed` (`bin\fsed.exe` on Windows); there is nothing else to install. Qt, FFmpeg, BestSource, libass, the Aegisub automation scripts and the Polish and English spelling dictionaries are inside. AviSynth+ or VapourSynth are needed only to open their scripts. The packages were built by CI from the tagged commit; verify a download against `SHA256SUMS.txt` (`sha256sum -c SHA256SUMS.txt`, or `certutil -hashfile <file> SHA256` on Windows).
