# Kainote UI reference spec (for FS Editor layout parity)

Status: research notes, 2026-10-13. Target: FS Editor (Rust core + Qt 6 Widgets GUI, BSD-3).

## 0. Licence and clean-room rules

Kainote (https://github.com/bjakja/Kainote) is C++/wxWidgets software under **GPL-3.0**. FS Editor is BSD-3, so:

- **No Kainote code, resources, icons or translations may be copied.** That includes source, `.mo`/`.po` catalogues, theme files and icon bitmaps.
- This document records only **what exists**: the names of UI elements, their order and placement, the observable behaviour, the shortcuts and the on-disk file markers. Everything is described in our own words.
- Translation strings listed below are only *identifiers of UI elements*, so the Kainote layout can be recognised. The FS Editor string tables must be written independently. Neutral wording that happens to coincide is fine; bulk-importing Kainote's catalogue is not.
- **File-format compatibility.** The `[Script Info]` keys and line markers Kainote writes are interoperability facts, not copyrightable expression. FS Editor may read and write them so files round-trip between the two editors.

Sources used:

- **Help (Polish), the main behavioural source:**
  - https://bjakja.github.io/index.html, with sub-pages cited per section
  - https://bjakja.github.io/Conowego.html (changelog)
- **Repository README and sources.** Read only to enumerate menu items, order and markers:
  - https://github.com/bjakja/Kainote
  - https://raw.githubusercontent.com/bjakja/Kainote/master/Kainote/
  - Files read: `KainoteFrame.cpp`, `TabPanel.cpp`, `SubsGridBase.cpp`, `SubsGrid.cpp`, `SubsGridWindow.cpp`, `EditBox.cpp`, `AudioBox.cpp`, `VideoBox.cpp`, `VideoToolbar.cpp`, `VideoFullscreen.cpp`, `ShiftTimes.cpp`, `Hotkeys.cpp`, `Notebook.cpp`, `KaiToolbar.cpp`, `FindReplaceDialog.cpp`, `SelectLines.cpp`, `ScriptInfo.cpp`, `StyleStore.cpp`, `StyleChange.cpp`, `SubsFile.cpp`, `SubsGridPreview.cpp`, `TLDialog.cpp`, `OptionsDialog.cpp`
- **Local install, inspected read-only:** `D:\Programy\Kainote`
  - `Config\Config.txt`, `Config\AudioConfig.txt`, `Config\LastSession.txt`
  - `Locale\en.mo`. In the installed build the msgids are Polish and the msgstrs are English, which gives the PL/EN pairs used below.
  - `Themes\*.txt`
  - Autosaves in `Subs\`

---

## 1. Main window

### 1.1 Overall structure

```
+--------------------------------------------------------------------------------+
| Menu bar: File  Edit  Video  Audio  View  Subtitles  Automation  Help          |
+---+----------------------------------------------------------------------------+
| T |  TAB CONTENT (one "TabPanel" per open subtitle file)                       |
| O |  +--------------------------+------------------------------------------+   |
| O |  |                          | [Audio box - only when audio is loaded]  |   |
| L |  |  VIDEO BOX               |------------------------------------------|   |
| B |  |  (video + seek slider +  | EDIT BOX (tag bar, info row, text,       |   |
| A |  |   buttons + visual-tool  |  fields row)                             |   |
| R |  |   toolbar)               |                                          |   |
|   |  +--------------------------+------------------------------------------+   |
| ( |  ===================== horizontal resizer (drag) =======================   |
| v |  +------------------------------------------------------+-------------+   |
| e |  |  SUBTITLE GRID                                       | SHIFT TIMES |   |
| r |  |                                                      | panel       |   |
| t |  |                                                      | (optional)  |   |
| . |  +------------------------------------------------------+-------------+   |
|   |  [ tab1.ass  x ][ tab2.ass ][ + ]          <- tab bar at the BOTTOM        |
+---+----------------------------------------------------------------------------+
| status | scale% | zoom% | duration | fps | video res | AR | subs res | file    |
+--------------------------------------------------------------------------------+
```

- **Menu bar.** Kainote draws its own (`KaiMenuBar`), so it can be themed.
- **Toolbar** (`KaiToolbar`). By default it is a vertical icon strip on the **left** edge. Its position is set by `TOOLBAR_ALIGNMENT`: 0 = left (default), 1 = top, 2 = right, 3 = bottom. See section 3.
- **Tab notebook.** It fills the rest of the window, and its **tab bar sits at the bottom**.
- **Status bar.** It has 9 fields, described in 1.4.

### 1.2 Tab bar (bottom)

- One tab per open subtitle file. The label is the file name, with a `*` mark when the file is modified.
- **Mouse:**
  - The active tab shows a small close "x". Middle-clicking any tab closes it.
  - A "+" button after the last tab opens a new empty tab. Double-clicking the empty bar area does the same.
  - Tabs can be reordered by dragging.
  - When the tabs overflow, left/right arrow buttons appear. The mouse wheel over the bar also scrolls the tabs.
- **Tooltip:** the full subtitle file name and the video file name.
- **"Split view" ("Show two tabs").** Two tabs are shown side by side with a draggable vertical divider; each side is at least about 200 px. Clicking a side makes it the active tab.
- **Context menu of the tab bar.** The PL label is in parentheses.
  1. A radio list of all open tabs, used to switch.
  2. Save (Zapisz), Save all (Zapisz wszystko), Close all tabs (Zamknij wszystkie karty).
  3. Open the folder containing subtitles / video / audio / keyframes.
  4. Show one tab / Show two tabs.
  5. **Subtitle comparison** submenu (only when two or more tabs are open; the help says to right-click the *inactive* tab):
     - Compare by times. Only lines with identical start and end times are compared.
     - Compare by visible lines. Respects the current filtering.
     - Compare by selections.
     - Compare by styles.
     - Compare by selected styles. This opens a style checklist.
     - Compare. Runs the comparison with the options above.
     - Turn off comparison.

     Source: https://bjakja.github.io/Porownywanienapisow.html
- **Hotkeys:** Ctrl+T new tab, Ctrl+W close tab, Ctrl+PgDn next tab, Ctrl+PgUp previous tab.

### 1.3 Tab content layout and resizing

- **Top row:** `[Video box | Edit box]`.
  - The video box has a fixed width, computed from the stored video window size (`VIDEO_WINDOW_SIZE`). The edit box takes the rest of the width.
  - When audio is loaded, the audio box sits **inside the edit-box column, above the tag bar**. It has its own horizontal splitter, and its stored height is `AUDIO_BOX_HEIGHT` (default about 170 px).
- **Horizontal resizer** between the top row and the grid.
  - Dragging it changes the height of the top row, and with that the height of the video.
  - Minimum top-row height: about 350 px when video is visible, about 150 px otherwise.
  - Shift+drag keeps the video aspect ratio while resizing.
- **Bottom row:** `[Grid | Shift-times panel]`. The shift panel is docked on the **right** side of the grid and toggled by Ctrl+I. Whether it is shown is remembered in `SHIFT_TIMES_ON`.
- **View menu modes** (Widok). They change which panels are visible in the current tab:

  | Mode | EN | PL | Visible |
  |---|---|---|---|
  | 1 | All | Wszystko | video, audio, edit box, grid, shift panel |
  | 2 | Video and subs | Wideo i napisy | video, edit box, grid (audio hidden) |
  | 3 | Audio and subs | Audio i napisy | audio, edit box, grid (video hidden). The edit box gets a minimum of about 500x350 |
  | 4 | Only video | Tylko wideo | video only. The edit box, grid, shift panel and resizer are hidden |
  | 5 | Only subtitles | Tylko napisy | edit box and grid |

- **"Enable/Disable editor" (Ctrl+E), "player mode".**
  - Hides the grid, edit box, resizer and shift panel.
  - The video fills the tab, and the visual-tools toolbar row of the video box is hidden.
  - The main window is resized to fit the video.
  - Pressing Ctrl+E again restores the editor. Config key: `EDITOR_ON`.
- **Fullscreen** is a separate top-level window. Enter it with F, by double-clicking the video, or from the video context menu. Exit it with Esc. B is the "boss key": it leaves fullscreen and minimises.
  - Bottom panel layout: seek slider, then buttons Previous file / Play-Pause / Play line / Stop / Next file, a "Show toolbar" checkbox, a read-only time field, a video name label and a volume slider.
  - With multiple monitors, the context menu offers "Switch full screen to monitor N" and "Open in full screen on monitor N".

### 1.4 Status bar (9 fields, left to right)

1. Progress / message text.
2. Video scale in % (window size relative to native).
3. Video zoom in %, shown when zoom is active.
4. Video duration.
5. FPS.
6. Video resolution. Drawn in the **warning colour** when it differs from the subtitles' PlayResX/Y.
7. Video aspect ratio (display AR).
8. Subtitle resolution (PlayResX x PlayResY).
9. Video file name.

### 1.5 Video zoom (main-window level)

- Video > "Zoom video" (Powiększ wideo), also on the toolbar, shows a zoom rectangle over the video.
  - The rectangle can be dragged and resized.
  - Enter applies it. The changelog mentions zoom up to 700% of the window size.
  - "Turn off video zoom" (Ctrl+Shift+Z) resets it.
- The mouse wheel over the video, with the pointer tool active, zooms 100% to 1000% around the cursor.
- Ctrl+wheel over the video changes the height of the video window, which drives the resizer.
- The wheel over the video control panel changes the volume.
- Config key: `VIDEO_ZOOM_PERCENT`.

### 1.6 Session

- `Config\LastSession.txt` stores, per tab: Tab, Video, Position, FFMS2 flag, Subtitles, Active line, Scroll and Editor.
- Session files can also be saved and loaded with the `.kls` extension (File > Last session submenu).

---

## 2. Menus (EN / PL)

The top-level order is **File, Edit, Video, Audio, View, Subtitles, Automation, Help**. PL top-level labels, with the `&` accelerator: `&Plik`, `&Edycja`, `&Wideo`, `&Audio`, `Wido&k`, `&Napisy`, `Au&tomatyzacja`, `Pomo&c`.

Every menu item can carry an icon; the same icon is used on the toolbar. **Shift+click on any menu item** opens hotkey assignment for that item (since 0.9.1.700, per the changelog). Default shortcuts are shown after a tab in the label.

Separators below are inferred from the grouping in the source, and their positions are approximate.

### 2.1 File / Plik

| # | EN | PL | Default key |
|---|---|---|---|
| 1 | Open subtitles | Otwórz napisy | Ctrl+O |
| 2 | Save | Zapisz | Ctrl+S |
| 3 | Save all | Zapisz wszystko | |
| 4 | Save as... | Zapisz jako... | Ctrl+Shift+S |
| 5 | Save translation | Zapisz tłumaczenie | (TL mode only) |
| 6 | Recently opened subtitles (submenu) | Ostatnio otwarte napisy | |
| 7 | Remove subtitles from the editor | Usuń napisy z edytora | |
| 8 | Save subtitles using the video name (check) | Zapisuj napisy z nazwą wideo | |
| 9 | Open auto save | Otwórz autozapis | |
| 10 | Remove temporary files | Usuń pliki tymczasowe | |
| 11 | Show / Hide log window | Pokaż / Ukryj okno logów | |
| 12 | Last session (submenu) | Ostatnia sesja | |
| 13 | Settings | Ustawienia | |
| 14 | Exit (Alt-F4) | Wyjście | Alt+F4 |

The **Last session** submenu contains:

- Load last session
- Load session from file
- Save session to file
- Ask whether to load last session at startup (check)
- Load last session after program start (check)

### 2.2 Edit / Edycja

| # | EN | PL | Key |
|---|---|---|---|
| 1 | Undo | Cofnij | Ctrl+Z |
| 2 | Undo to last save | Cofnij do ostatniego zapisu | |
| 3 | Redo | Ponów | Ctrl+Y |
| 4 | History | Historia | Ctrl+Shift+H |
| 5 | Find and replace | Znajdź i zamień | Ctrl+H |
| 6 | Find | Znajdź | Ctrl+F |
| 7 | Find next | Znajdź następny | F3 |
| 8 | Sort all lines (submenu) | Sortuj wszystkie linie | |
| 9 | Sort selected lines (submenu) | Sortuj zaznaczone linie | |
| 10 | Fix minor errors (experimental) | Popraw drobne błędy (eksperymentalne) | |
| 11 | Select lines | Zaznacz linijki | |

Both sort submenus sort by: start time, end time, styles, actor, effect, layer.

### 2.3 Video / Wideo

| # | EN | PL | Key |
|---|---|---|---|
| 1 | Open video | Otwórz wideo | Ctrl+Shift+O |
| 2 | Recently opened videos (submenu) | Ostatnio otwarte wideo | |
| 3 | Open keyframes | Otwórz klatki kluczowe | |
| 4 | Recently opened keyframes (submenu) | Ostatnio otwarte klatki kluczowe | |
| 5 | Open dummy video | Otwórz dummy wideo | |
| 6 | Insert start time from video | Wstaw czas początkowy z wideo | Ctrl+Left |
| 7 | Insert end time from video | Wstaw czas końcowy z wideo | Ctrl+Right |
| 8 | Previous frame | Klatka w tył | Left |
| 9 | Next frame | Klatka w przód | Right |
| 10 | Go to start time of line | Przejdź do czasu początkowego linii | |
| 11 | Go to end time of line | Przejdź do czasu końcowego linii | |
| 12 | Play/Pause | Odtwórz / Pauza | Alt+Space |
| 13 | Go to previous keyframe | Przejdź do poprzedniej klatki kluczowej | |
| 14 | Go to next keyframe | Przejdź do następnej klatki kluczowej | |
| 15 | Set audio position to video time | Ustaw audio z czasem wideo | |
| 16 | Set audio marker to video time | Ustaw znacznik audio z czasem wideo | |
| 17 | Zoom video | Powiększ wideo | |
| 18 | Turn off video zoom | Wyłącz powiększenie wideo | Ctrl+Shift+Z |
| 19 | Open video with FFMS2 (check) | Otwieraj wideo przez FFMS2 | |

### 2.4 Audio

| # | EN | PL |
|---|---|---|
| 1 | Open audio | Otwórz audio |
| 2 | Recently opened audio (submenu) | Ostatnio otwarte audio |
| 3 | Open audio from video | Otwórz audio z wideo |
| 4 | Open blank 2h30m audio | Otwórz puste audio na 2:30 godziny |
| 5 | Close audio | Zamknij audio |

### 2.5 View / Widok

All / Wszystko; Video and subs / Wideo i napisy; Audio and subs / Audio i napisy; Only video / Tylko wideo; Only subtitles / Tylko napisy. See the table in 1.3.

### 2.6 Subtitles / Napisy

| # | EN | PL | Key |
|---|---|---|---|
| 1 | Enable/Disable editor | Włącz / Wyłącz edytor | Ctrl+E |
| 2 | ASS file properties | Właściwości pliku ASS | |
| 3 | Style manager | Menedżer stylów | Ctrl+M |
| 4 | Conversion (submenu) | Konwersja | |
| 5 | Shift times... | Okno zmiany czasów | Ctrl+I |
| 6 | Font collector | Kolekcjoner czcionek | |
| 7 | Resample subtitles | Zmień rozdzielczość napisów | |
| 8 | Check spelling | Sprawdź poprawność pisowni | |
| 9 | Hide tags | Ukryj tagi w nawiasach | |

The **Conversion** submenu holds one entry per format. The entry for the current format is disabled.

| EN | PL | Key |
|---|---|---|
| Convert to ASS | Konwertuj do ASS | F9 |
| Convert to SRT | Konwertuj do SRT | F8 |
| Convert to MDVD | Konwertuj do MDVD | F10 |
| Convert to MPL2 | Konwertuj do MPL2 | F11 |
| Convert to TMP | Konwertuj do TMP | Ctrl+F12 |

### 2.7 Automation / Automatyzacja

1. Load script (Wczytaj skrypt).
2. Refresh autoload scripts (Odśwież skrypty autoload).
3. Run the last loaded script (Uruchom ostatnio zaczytany skrypt).
4. Open shortcut mapping window (Otwórz okno mapowania skrótów). This assigns hotkeys to automation macros.
5. A separator, then one entry per registered macro. The autoload macros are grouped by script.

### 2.8 Help / Pomoc

1. Help (Pomoc). Opens https://bjakja.github.io/index.html. Hotkey F1.
2. Forum thread on animesub.info.
3. Check for updates.
4. About (O programie).
5. Credits / Thanks.

---

## 3. Toolbar

- **Placement.** Vertical, on the left, by default. It can be moved to any window edge; the position is stored in `TOOLBAR_ALIGNMENT`.
- **Buttons:**
  - Square, about text height + 8 px, icon only, with a tooltip that shows the menu label plus the hotkey.
  - If the buttons do not fit, they wrap into extra columns or rows.
  - Every toolbar button is just a menu command (same ID, icon and hotkey).
  - A button whose menu item has a submenu (for example the recent lists) pops that submenu up.
- **Default content** (from `TOOLBAR_IDS` in Config.txt, in order):

| # | Command | Notes |
|---|---|---|
| 1 | Open subtitles | |
| 2 | Recently opened subtitles | submenu popup |
| 3 | Open video | |
| 4 | Recently opened videos | submenu popup |
| 5 | Save | |
| 6 | Save as | |
| 7 | Save all | |
| 8 | Remove subtitles from the editor | |
| 9 | Enable/Disable editor | |
| 10 | Find and replace | |
| 11 | Style manager | |
| 12 | ASS file properties | |
| 13 | Shift times | |
| 14 | Go to start time (video to line start) | |
| 15 | Go to end time (video to line end) | |
| 16 | Insert start time from video | |
| 17 | Insert end time from video | |
| 18 | Convert to ASS | |
| 19 | Convert to SRT | |
| 20 | Zoom video | |
| 21 | Resample subtitles | |
| 22 | Hide tags | |
| 23 | Settings | |
| 24 | (half-size arrow) "Choose toolbar icons" / "Wybierz ikony paska narzędzi" | always last |

- **Customisation popup.** The trailing arrow opens a popup that lists every menu command, each with its icon, label and a check mark when it is on the toolbar.
  - Clicking an entry toggles it on or off the toolbar.
  - The popup also contains a "Toolbar alignment" choice: Left / Top / Right / Bottom (PL: Po lewej / U góry / Po prawej / Na dole).
  - Toolbar buttons can be reordered by dragging.
  - The result is persisted as an ordered ID list.
- **Help:** https://bjakja.github.io/Paseknarzedzi.html
- **FS Editor note.** Use our own icon set. Persist the order as a list of stable command IDs (our own names).

---

## 4. Subtitle grid

### 4.1 Columns

Columns for an ASS file, left to right. The PL header is in parentheses.

| Column | EN header | PL header | Content |
|---|---|---|---|
| Number | # | # | 1-based line index; its background shows the line state (4.2) |
| Layer | L. | W. | layer |
| Start | Start | Start | start time (or frame in frame mode) |
| End | End | Koniec | end time |
| Style | Styles | Style | style name |
| Actor | Actor | Aktor | actor/name field |
| MarginL | Left | M.L. | left margin |
| MarginR | Right | M.P. | right margin |
| MarginV | Vert. | M.Pi. | vertical margin |
| Effect | Effect | Efekt | effect |
| CPS | CPS | ZNS | characters per second |
| Wraps | Wraps | Łamania | per-visual-line char counts, e.g. `38/12` |
| Text | Text | Tekst | text, with tags shown or hidden (4.4) |

- **Translation mode.** When the original is shown, the Text column splits into **"Original text" (Tekst oryginalny)** and **"Translation" (Tekst tłumaczenia)**.
- **SRT** files show only `# Start End CPS Wraps Text`. **TMP** shows `# Start Wraps Text`. MDVD and MPL2 behave like SRT.
- **Auto-hidden columns.** Per the help, the layer, actor, margin and effect columns are shown automatically **only when at least one line has a non-default value**. The user can also hide any column explicitly (4.6, "Hide columns").
- **Grid font:** `GRID_FONT` (default `Tahoma, 10`).

### 4.2 Colours and row states

Theme roles. The names come from the theme file format, used here only as a role list.

- **Line background:**
  - Dialogue background: `GRID_DIALOGUE`.
  - Comment background: `GRID_COMMENT`.
  - Selection: `GRID_SELECTION`, drawn as a translucent overlay (the colour has alpha).
  - Lines **visible on video at the current video time** are tinted with `GRID_LINE_VISIBLE_ON_VIDEO`.
- **Number cell state:**

  | State | Role | Default look |
  |---|---|---|
  | normal | `GRID_LABEL_NORMAL` | bluish |
  | modified since load | `GRID_LABEL_MODIFIED` | yellow-green |
  | modified and saved | `GRID_LABEL_SAVED` | light |
  | doubtful (TL mode) | `GRID_LABEL_DOUBTFUL` | orange |

- **Active line.** A thick frame is drawn around the active line's number cell (`GRID_ACTIVE_LINE`). The "current" line gets a thin frame.
- **Collisions.** The text of lines that overlap the active line in time is drawn in `GRID_COLLISIONS`.
- **Error highlights** use `GRID_SPELLCHECKER` or a red background. They apply to spelling errors (red background behind the word), an unknown style name, CPS above 15 (in the CPS cell) and too many or too long wraps (in the Wraps cell).
- **Comparison colours:** `GRID_COMPARISON_*` (outline, background, comment background, background not matching).
  - The help describes navy for matching text and light brown for differing text.
  - In the preview, differing lines get an outline whose colour depends on the theme.
- **Header row:** `GRID_HEADER`, `GRID_HEADER_TEXT`.
- **Filtered blocks.** Hidden ranges show a small "+" square in a narrow strip at the left of the number column. Clicking it expands or collapses that hidden block.
- **Tree rows** ("Make tree") show an arrow / expand marker and a description row.
- **Subtitle preview.** Ctrl+Q shows an embedded mini-grid from another tab with the lines that overlap the active line in time. The matching lines are highlighted in blue in the number column. Right-clicking the preview picks other matches. Source: https://bjakja.github.io/Podgladnapisow.html

### 4.3 Mouse and keyboard

- **Left click.** Selects the line and makes it active, loading it into the edit box. Ctrl toggles the line in the selection; Shift extends the range.
- **Number column:**
  - Click: the video seeks to the line start. Shift+click seeks to the line end.
  - Right-click: places the **sync marker** used by Shift Times (4.7).
- **Click or double-click on the Start/End cell:** the video seeks to that time.
- **Double-click on a line:** the video seeks to the line (always, independent of the auto-seek option).
- **Middle click:** plays the line. Ctrl+middle click sets the line active without changing the selection.
- **Alt+drag:** moves the selected rows to another position.
- **Wheel:** scrolls 3 rows.
- **Keys:**
  - Up/Down, PgUp/PgDn, Home/End move the selection; Shift+arrows extend it; Ctrl+A selects all.
  - Alt+Up / Alt+Down move the selected rows.
  - Enter moves focus to the edit-box text.

### 4.4 Tag hiding

Subtitles > "Hide tags" replaces every `{...}` block in the Text column with a single swap character. The character is configurable as `GRID_TAGS_SWAP_CHARACTER`, default `☀`.

### 4.5 Filtering

Source: https://bjakja.github.io/Filtrowanie.html

- Filtering hides lines instead of deleting them. Hidden blocks are marked by the "+" strip (4.2).
- **Criteria:** hide lines with chosen styles, hide selected lines, hide comments, show only unconfirmed lines (TL mode), show only untranslated lines (TL mode).
- **Options:**
  - "Reverse filtering" inverts the result.
  - "Do not reset previous filtering" adds to the existing filtering instead of replacing it.
  - "Filter after loading subtitles" re-applies the filtering when a file is opened.
- **"Ignore filtering in some actions"** is a check. When on, operations such as shifting can include hidden lines.

### 4.6 Grid context menu (right-click on rows)

In order:

1. **Insert lines** (submenu):
   - Insert before
   - Insert after
   - Insert before with video time
   - Insert after with video time
   - Insert before with video frame time
   - Insert after with video frame time
2. Duplicate lines (Ctrl+D)
3. Swap lines. Enabled only when exactly 2 lines are selected.
4. Join lines. Enabled for 2 to 20 lines.
5. Join and keep first
6. Join and keep last
7. Set times as continuous (previous line)
8. Set times as continuous (next line)
9. **Split lines** (submenu):
   - at video time
   - into frames
   - into characters
   - into words
   - by wraps (`\N`)
10. Select all lines visible on video
11. Copy (Ctrl+C), Cut (Ctrl+X), Paste (Ctrl+V)
12. Copy columns (Ctrl+Shift+C) and Paste columns (Ctrl+Shift+V). Both open a column-choice dialog with checkboxes:
    - Layer, Start, End, Actor, Styles, MarginL, MarginR, MarginV, Effect, Text.
    - In TL mode, Text into original / into translation.
    - For copy only, "Text without tags".
13. **Hide columns** (submenu of checks): layer, start, end, actor, style, left margin, right margin, vertical margin, effect, CPS, line wraps
14. Hide selected lines
15. **Filtering** (submenu):
    - Filter after loading subtitles (check)
    - Reverse filtering (check)
    - Do not reset previous filtering (check)
    - Hide lines with styles. This is a submenu with one check per style.
    - Hide selected lines (check)
    - Hide comments (check)
    - Show unconfirmed (TL only)
    - Show untranslated (TL only)
    - Filter (apply)
    - Turn off filtering
16. Ignore filtering in some actions (check)
17. Make tree. This groups the selected lines under a collapsible description row.
18. Show subtitles preview (Ctrl+Q)
19. Set new FPS
20. Set FPS from video
21. Paste translation text (TL mode; see 5.4)
22. Dialogue shifting window (TL mode, after a translation has been pasted)
23. separator
24. Delete text (Alt+Del). Clears the text but keeps the lines.
25. Delete (Shift+Del). Removes the lines.
26. separator
27. Font collector
28. Load subtitles from an MKV/OGM file

**Tree row context menu:** Add lines, Copy tree, Change description, Select tree lines, Delete.

### 4.7 Shifting times "from the grid"

Kainote has **no drag-to-shift in the grid**. Time shifting is done with the docked **Shift Times panel** to the right of the grid (section 8), together with a **sync marker** in the grid.

1. Right-click the number cell of a line to place the marker.
2. In the panel, choose "Shift by video/audio" plus Beginning or End.
3. Press Shift. The offset becomes the marker line's time minus the current video (or audio) time.

The options "Move the marker to video time" and "Move the marker to audio time" move the marker to the current position after shifting.

---

## 5. Translation mode (TL)

Sources:

- Help: https://bjakja.github.io/Trybtlumaczenia.html
- Behaviour, verified on autosave files in `D:\Programy\Kainote\Subs\` and against the source (`SubsFile.cpp`, `SubsGridBase.cpp` load/save, `EditBox.cpp`)

### 5.1 Concept

Each dialogue line carries **two texts**: the foreign *original* (shown in the upper, read-only-ish editor) and the *translation* (the main editor). Translation mode is switched on with the **"Translator mode" (Tryb tłumacza)** checkbox in the edit box info row. It is available only for ASS. Files in other formats are converted first.

### 5.2 Storage format inside .ass (interoperability spec)

**`[Script Info]` keys written by Kainote.** Values are as seen in the autosave files.

```
; Script generated by Kainote <version>
...
TLMode: Yes                 ; TL mode on.  Value "Translated" = translation finished
TLMode Style: TLmode        ; name of the style used to mark ORIGINAL lines
TLMode Showtl: Yes          ; optional; set after "Paste translation text",
                            ; enables the "Dialogue shifting window" item
```

**`[V4+ Styles]`:** the TL style (default name `TLmode`) is added as a **copy of the first style with alignment 8** (top-centre). This lets the original be seen above the translation in other players.

**`[Events]` encoding:**

- A line that **has a translation** (or is marked doubtful) is written as **two consecutive Dialogue lines**:
  1. the **original**, with `Style` = the TL style (`TLmode`). Its other fields are copied from the line.
  2. immediately after it, the **translation** line, with the line's real style and fields.
- A line **without a translation** is written **once**, with its normal style. Its single text counts as the original/foreign text.
- **On load:** when the TL keys are present, a Dialogue line whose style equals the TL style is taken as the *original*, and the **next** event in the file is its *translation*. Both are merged into one grid row.
- **Doubtful / unconfirmed marker:** the translation line's **Effect** field gets a form-feed control character (U+000C) followed by `D` appended.
  - In C-string notation the appended marker is `"\fD"`.
  - It is invisible in most editors and must be stripped when the Effect is displayed.
  - In the grid, the line's number cell is coloured with the "doubtful" role.
- Verified sample. An autosave with `TLMode: Yes` whose lines contained no translations had **no** TLmode Dialogue lines at all. This is consistent with the "write once when untranslated" rule.

**Other `[Script Info]` keys Kainote adds.** These are not TL-specific but are worth preserving:

| Key | Meaning |
|---|---|
| `Video File` | path of the associated video |
| `Audio File` | path of the associated audio |
| `Keyframes File` | path of the associated keyframes |
| `Video AR Mode` | aspect-ratio override mode |
| `Video AR Value` | aspect-ratio override value |
| `Video Zoom Percent` | video zoom state |
| `Scroll Position` | grid scroll position |
| `Active Line` | active grid line |
| `Video Position` | last video position |
| `Last Style Storage` | name of the style catalogue last used |

**Actor-field prefixes used for grid features** (stored inside the Actor/Name field):

| Prefix | Purpose |
|---|---|
| `[bookmark]` | bookmark |
| `[hidden]` | line hidden by filtering |
| `[visible]` | line visible under filtering |
| `[tree_description]` | tree description row |
| `[tree_opened]` | expanded tree state |
| `[tree_closed]` | collapsed tree state |

FS Editor should parse these prefixes, not show them in the Actor column, and write them back.

**Switching TL mode off** asks for confirmation, with a warning that the foreign text will be deleted. The translation then replaces the text, and the TL keys and the TLmode style are removed.

**File > "Save translation"** writes only the translated text as a normal ASS. The TL keys are dropped and only the translation lines are kept.

### 5.3 Edit-box layout in TL mode

```
+--------------------------------------------------------------------+
| tag bar (B I U S colours an T1..Tn)                                |
| Line: 12   Wraps: 38/12   CPS: 14<=15   [x] Translator mode  (o)Time ( )Frames |
+--------------------------------------------------------------------+
| ORIGINAL TEXT editor (foreign)                                     |
+--------------------------------------------------------------------+
| [Paste the selected] [Paste all] [ ] Hide original                 |
| [ ] Not confirmed    [ ] Moving tags                               |
+--------------------------------------------------------------------+
| TRANSLATION TEXT editor (main)                                     |
+--------------------------------------------------------------------+
| fields row (Comment Layer Start End Duration Style Actor M M M Eff)|
+--------------------------------------------------------------------+
```

| Control | EN | PL | Behaviour |
|---|---|---|---|
| button | Paste the selected | Wklej zaznaczone | copies the selected original text into the translation |
| button | Paste all | Wklej wszystko | copies the whole original into the translation |
| toggle | Hide original | Ukryj oryginał | hides the original on the video render |
| toggle | Not confirmed | Niepewne | marks the line doubtful (the `\fD` Effect marker) |
| toggle | Moving tags | Przenoszenie tagów | when on, tags are carried from the original to the translation |

**TL-specific hotkeys (Editbox context):**

- Ctrl+D: go to the next doubtful line.
- Ctrl+R: go to the next untranslated line.
- Alt+Down: set doubtful and go to the next line.

The messages "No unconfirmed lines found" and "No untranslated lines found" appear when nothing is left.

### 5.4 Paste translation text / dialogue shifting window (TLDialog)

- **Grid > "Paste translation text."** This loads a plain translated text, one line per dialogue, into the translation column.
- Then the **"Translation matching options"** dialog ("Dialogue shifting window") shows two parallel lists: *Original* on the left and *Translation* on the right.
  - Each side has buttons: Add line, Join lines, Delete line.
  - These realign the two texts when the line counts differ.
- The key `TLMode Showtl: Yes` keeps the item enabled after saving.

---

## 6. Video panel and visual tools

Sources: https://bjakja.github.io/Pracazwideo.html and https://bjakja.github.io/Narzedziaedycjiwizualnej.html

### 6.1 Layout

```
+--------------------------------------------------------------+
|                                                              |
|                     VIDEO (render + subs)                    |
|                                                              |
+--------------------------------------------------------------+
| ====================== seek slider ========================= |  (chapters shown as marks)
| [|<][>||][>line][#][>|]  [ 0:01:23,456; 1234; 12; 120ms, -80ms ]  [--volume--] |
| [pointer][pos][move][scale][rotZ][rotXY][rclip][vclip][draw][shift][hydra] | sub-toolbar | [Seek on: v][Play: v] |
+--------------------------------------------------------------+
```

- **Seek slider.** Spans the full width. Chapter markers are drawn on it. It can be hidden from the context menu ("Show / hide progress bar").
- **Button row.** Buttons are about 26 px. Tooltips, EN / PL:
  - Previous file / Poprzedni plik
  - Play / Pause / Odtwórz / Pauza
  - Play the current line / Odtwórz aktywną linię
  - Stop / Zatrzymaj
  - Next file / Następny plik
- **Time field.** Read-only. It shows `h:mm:ss,cs` (milliseconds), then the frame number, the frames since the active line's start, and the difference in ms to the line start and to the line end. Its background switches to the warning colour when the frame is a keyframe.
- **Volume slider** on the right. The wheel over the panel changes the volume.
- **Visual toolbar row** (the "VideoToolbar"). It is hidden in player mode.

### 6.2 Visual tools (left icon group, 11 tools)

| # | EN | PL | ASS target |
|---|---|---|---|
| 1 | Position pointer (crosshair) | Wskaźnik pozycji | none. Shows coordinates; middle-click or Ctrl+click inserts `\pos` |
| 2 | Text positioning | Przesuwanie tekstu | `\pos` |
| 3 | Text moving | Ruch | `\move` |
| 4 | Text scaling | Skalowanie | `\fscx` `\fscy` |
| 5 | Z rotation | Obrót wokół osi Z | `\frz` (+ `\org`) |
| 6 | X/Y rotation | Obrót wokół osi X / Y | `\frx` `\fry` |
| 7 | Rectangle clipping | Wycinki prostokątne | `\clip(x1,y1,x2,y2)` / `\iclip` |
| 8 | Vector clipping | Wycinki wektorowe | `\clip(drawing)` |
| 9 | Vector drawing | Rysunki wektorowe | `\p1` drawings |
| 10 | Position shifter | Zmieniacz pozycji | shifts all positional tags at once |
| 11 | Hydra | Hydra | multi-line tag editor / gradients |

**Sub-toolbar for the active tool** (shown right after the tool icons):

- **Vector clip / drawing:**
  - Move points, Add line, Add Bézier curve, Add B-spline, Add separate point (move), Delete point, Invert clip (clip only).
  - Drawing also has a list of ready-made shapes with an "Edit" button to manage them.
- **Position shifter.** Toggles for which tags are affected: `\pos`, `\move` start, `\move` end, clips, drawings, `\org`.
- **Z rotation:** "angle from 2 points", "change all Z tags", "preserve proportions".
- **Scale:**
  - "set scale by rectangle", "scale width", "preserve aspect ratio", "scale height".
  - "custom original rectangle", "change all scale tags", "preserve proportions".
- **Positioning:** "position by rectangle", "X axis only", "Y axis only", plus an alignment list with 21 entries (anchor variants).
- **Move:** "set movement from 2 points and video" (times taken from the video).
- **X/Y rotation:** "change all X/Y tags".
- **Hydra:**
  - Tag list.
  - Mode list: Add, Insert, Multiply, Multiply+, Gradient text increasing, Gradient text decreasing, Gradient line increasing, Gradient line decreasing.
  - Edit button.

**Right side: two combo boxes.**

1. **"Move video to selected line on:"**
   - Double-clicking a line (always active)
   - Every line change
   - Clicking a line or editing when paused
   - Clicking a line or editing
   - Editing line when paused
   - Editing
2. **"On moving to another line play:"**
   - Nothing
   - Audio to line end
   - Video and audio to line end
   - Video and audio to next line start

### 6.3 Mouse behaviour on video (from the help)

- **Positioning:** drag the anchor. Shift constrains movement to one axis.
- **Move:** left-click sets the start point and right-click sets the end point; both can be dragged.
- **Scale:** left-drag scales X, right-drag scales Y, middle-drag scales both.
- **X/Y rotation:** left rotates around Y, right rotates around X, middle rotates both.
- **Helper lines:** the middle button toggles helper lines for positioning and move (changelog).
- **Keyboard nudge:** WSAD moves the pos/move point by 1 px. Shift+WSAD moves it by 0.1.
- **Snapping:** Alt+drag in clip and drawing tools snaps to existing points.
- **Wheel:** zooms the video while the pointer tool is active (1.5).
- **Double-click:** enters fullscreen.

### 6.4 Video context menu

In order:

1. Copy video position (Kopiuj pozycję na wideo)
2. Play / Pause
3. Stop
4. Full screen (F), or Exit full screen (Escape)
5. Switch full screen to monitor N / Open in full screen on monitor N (one entry per extra monitor)
6. Open editor (only in player mode)
7. Recently opened subtitles (submenu)
8. Recently opened videos (submenu)
9. Open video
10. Open subtitles
11. Show / hide progress bar
12. Change aspect ratio
13. Save frame with subtitles as PNG
14. Copy frame with subtitles to clipboard
15. Save frame as PNG
16. Copy frame to clipboard
17. separator
18. Remove video. Moves the loaded video file to the recycle bin, after confirmation.
19. Filters (DirectShow only). Shows the filters in use.

---

## 7. Audio panel

Sources:

- https://bjakja.github.io/Pracazaudio.html
- https://bjakja.github.io/Tworzeniepodzialukaraoke.html
- `Config\AudioConfig.txt`

### 7.1 Layout

```
+-------------------------------------------------------------------+---+---+---+
| time scale (ruler)                                                | H | V | V |
|-------------------------------------------------------------------| z | z | o |
|  waveform / spectrum; selection between red (start) and orange    | o | o | l |
|  (end) boundaries; other lines' boundaries; keyframes (green);    | o | o | u |
|  audio mark (magenta); karaoke syllable splits (yellow)           | m | m | m |
|-------------------------------------------------------------------|   |   | e |
| ===================== search bar (scroll) ======================= |  [link]   |
+-------------------------------------------------------------------+-----------+
| [<line][line>][>syl][>line][#] [<mark][mark>] [<s][s>][<e][e>][>end] [+in][+out] [Commit][Goto] [K][Ks] [AC][NL][AS][Spec][Enh] |
+--------------------------------------------------------------------------------+
```

**Sliders on the right:**

| Slider | EN | PL |
|---|---|---|
| horizontal zoom | Horizontal stretching | Rozciągnięcie w poziomie |
| vertical zoom | Vertical stretching | Rozciągnięcie w pionie |
| volume | Volume | Głośność |

Below the sliders is a toggle, "Link the volume and stretch sliders". The scrollbar under the display is the "Search bar" (Pasek szukania).

### 7.2 Button row (left to right; 8 px gaps between the groups)

| # | Tooltip EN | PL |
|---|---|---|
| 1 | Play the previous line | Odtwórz poprzednią linijkę |
| 2 | Play the next line | Odtwórz następną linijkę |
| 3 | Play the current syllable / line | Odtwórz aktywną sylabę / linię |
| 4 | Play the current line | Odtwórz aktywną linię |
| 5 | Stop playback | Zatrzymaj odtwarzanie |
| 6 | Play before the mark | Odtwórz przed znacznikiem |
| 7 | Play after the mark | Odtwórz po znaczniku |
| 8 | Play 500 ms before the start time | Odtwórz 500ms przed czasem startowym |
| 9 | Play 500 ms after the start time | Odtwórz 500ms po czasie startowym |
| 10 | Play 500 ms before the end time | Odtwórz 500ms przed czasem końcowym |
| 11 | Play 500 ms after the end time | Odtwórz 500ms po czasie końcowym |
| 12 | Play to the end | Odtwórz do końca |
| 13 | Add lead-in to the active line | Dodaj wstęp do aktywnej linijki |
| 14 | Add lead-out to the active line | Dodaj zakończenie do aktywnej linijki |
| 15 | Apply changes (commit) | Zatwierdź zmiany |
| 16 | Go to selection | Przejdź do zaznaczenia |
| 17 | Enable / disable karaoke creation (toggle) | |
| 18 | Enable / disable automatic splitting of syllables (toggle) | Włącz / Wyłącz automatyczne dzielenie sylab |
| 19 | Automatically apply changes (toggle) | |
| 20 | Go to the next line after applying changes (toggle) | |
| 21 | Auto-scroll to the active line (toggle) | Automatyczne przewijanie do aktywnej linijki |
| 22 | Spectrum mode (toggle) | Tryb spektrum |
| 23 | Enhance speech frequencies in the spectrum (toggle) | |
| - | stretch spacer | |

### 7.3 Display colours (theme roles)

| Element | Colour |
|---|---|
| Start boundary | red |
| End boundary | orange |
| Inactive-line boundaries | dimmer variants |
| Audio mark | magenta |
| Keyframes | green |
| Playback cursor | single line |
| Selection background | modified and normal variants |
| Karaoke syllable lines | yellow, with the active syllable outlined in red |

Inactive lines are drawn according to `AUDIO_INACTIVE_LINES_DISPLAY_MODE`: none, previous+next, or all visible.

### 7.4 Mouse

- **Selection:** left click sets the start; right click sets the end. The boundaries can be dragged.
- **Seeking:** Ctrl+left click or middle click seeks the video to that point.
- **Wheel:**
  - Plain wheel scrolls.
  - Shift+wheel zooms horizontally. `AUDIO_WHEEL_DEFAULT_TO_ZOOM` swaps wheel and Shift+wheel.
  - Ctrl+wheel changes the vertical zoom.
- **Time ruler:** right-click sets the audio mark; dragging scrolls.
- **Snapping:** Shift temporarily toggles snapping to keyframes and other lines (`AUDIO_SNAP_TO_KEYFRAMES`, `AUDIO_SNAP_TO_OTHER_LINES`).

### 7.5 Karaoke mode

- Turning karaoke on widens the timeline by about 20%.
- Syllables are separated by yellow lines, and each syllable's text is drawn above its segment.
- **Auto-split** splits the text automatically (useful for Japanese romaji). With it off, splits are made by hand.
- **Mouse:**
  - Hover over the text: a red preview line appears; a left click splits there.
  - Middle-click on a boundary removes it. Middle-click on a syllable plays it.
  - Drag a boundary to retime it. A right-drag also moves all the following syllables.
- **Line start:** a left-drag moves only the start; a right-drag also moves the syllables.
- **Line end:** snaps to the last syllable.
- **Keys:** Left is the previous syllable, Right the next, Down plays the active syllable.

### 7.6 Relevant AudioConfig keys

| Key | Purpose | Default |
|---|---|---|
| `AUDIO_AUTO_COMMIT` | commit changes automatically | |
| `AUDIO_NEXT_LINE_ON_COMMIT` | move to the next line after a commit | |
| `AUDIO_KARAOKE` | karaoke mode | |
| `AUDIO_KARAOKE_SPLIT_MODE` | automatic syllable splitting | |
| `AUDIO_SPECTRUM_ON` | spectrum display | |
| `AUDIO_LEAD_IN_VALUE` | lead-in length | 200 ms |
| `AUDIO_LEAD_OUT_VALUE` | lead-out length | 300 ms |
| `AUDIO_MARK_PLAY_TIME` | play length around the mark | 1000 ms |
| `AUDIO_INACTIVE_LINES_DISPLAY_MODE` | how inactive lines are drawn | |
| `AUDIO_SNAP_TO_KEYFRAMES` | snap boundaries to keyframes | |
| `AUDIO_SNAP_TO_OTHER_LINES` | snap boundaries to other lines | |
| `AUDIO_WHEEL_DEFAULT_TO_ZOOM` | swap wheel and Shift+wheel | |
| `AUDIO_BOX_HEIGHT` | audio box height (in Config.txt) | |

Audio hotkeys are listed in section 10.

---

## 8. Shift Times panel

Source: https://bjakja.github.io/Przesuwanieczasownapisowipostpro.html

- **Placement:** docked to the **right of the grid**, as a narrow vertical panel with a scrollbar when it is too tall.
- **Toggle:** Ctrl+I, or Subtitles > "Shift times..." (Okno zmiany czasów).
- **Header:** a button at the top switches the panel between **"Shift times"** (Przesuwanie czasów) and **"Post processor"**.

### 8.1 Shift times mode (top to bottom)

1. **Profile box.** A `+` button saves the current settings as a profile, `-` deletes the profile, and a combo selects one.
2. **Time box:**
   - Time field (h:mm:ss,cc; or frames).
   - **[Shift]** button (Przesuń). It is the main action.
   - Radios: **Forward** / **Backward** (W przód / W tył).
   - Check **Frames** (Klatki): shift by frames instead of time.
   - Check **Tag times** (Czasy tagów): also shift times inside `\move`, `\t`, `\fad` and similar tags. ASS only.
3. **"Shift by video/audio" box:**
   - Check to enable it.
   - Radios **Beginning** / **End**: which time of the sync-marker line is matched to the video or audio position.
   - Check "Move the marker to video time" and check "Move the marker to audio time".
4. **"Which lines"** (Które linijki), radios:
   - All lines
   - Selected lines
   - From the selected line
   - All times higher and equal (times greater than or equal to the active line's)
   - All times lower and equal
   - According to the selected styles. Comes with a `+` button that opens a style checklist and a text field with the chosen styles.
5. **"A method of time shift"**, radios: Both times / The starting time / End time.
6. **"Correction of end times"**, radios: Leave unchanged / Adjust overlapping times / New times. The last one computes new end times from text length.

### 8.2 Post processor mode

1. **[Run post processor]** button.
2. **Lead-in / lead-out** box: check Lead-in with a ms field; check Lead-out with a ms field.
3. **"Set times as continuous"** box: check Enable; field Start threshold (ms); field End threshold (ms).
4. **"Snap to keyframes"** box:
   - Check Enable.
   - Fields: Before start, After start, Before end, After end (ms ranges).

The same "Which lines" selection and styles apply. Config keys: `SHIFT_TIMES_*` and `POSTPROCESSOR_*` (for example `POSTPROCESSOR_ON`, the lead-in/out values, thresholds and keyframe ranges).

---

## 9. Edit box

Source: https://bjakja.github.io/Edycjanapisow.html

### 9.1 Layout (top to bottom)

```
+------------------------------------------------------------------------------+
| [Audio box, if loaded]                                                        |
+------------------------------------------------------------------------------+
| [Font][B][I][U][S] [C1][C2][C3][C4] [an2 v]  [T1][T2][T3]...[Tn]             |  tag bar
| Line: 12   Wraps: 38/12   Characters per second: 14<=15  [x] Translator mode   |
|                                                         (o)Time ( )Frames     |  info row
+------------------------------------------------------------------------------+
| [original editor - TL mode only] + TL buttons (see 5.3)                       |
+------------------------------------------------------------------------------+
|                                                                              |
|   TEXT EDITOR (syntax-coloured)                                              |
|                                                                              |
+------------------------------------------------------------------------------+
| [x]Comment [Layer] [Start] [End] [Duration] [Style v][Edit] [Actor v]         |
| [MarginL][MarginR][MarginV] [Effect v]                                        |  fields row
+------------------------------------------------------------------------------+
```

### 9.2 Tag bar

| Control | EN | PL | Action |
|---|---|---|---|
| button | Font selection | Wybór czcionki | opens a font dialog, inserts `\fn` and `\fs` |
| toggle | Bold | Pogrubienie | `\b1/0` (Ctrl+B) |
| toggle | Italic | Pochylenie | `\i1/0` (Ctrl+I in the edit box) |
| toggle | Underline | Podkreślenie | `\u1/0` |
| toggle | Strikethrough | Przekreślenie | `\s1/0` |
| colour | Primary color | Kolor podstawowy | `\1c` |
| colour | Secondary color for karaoke | Kolor zastępczy do karaoke | `\2c` |
| colour | Border color | Kolor obwódki | `\3c` |
| colour | Shadow color | Kolor cienia | `\4c` |
| choice | Text position | Położenie tekstu | `\an1..9` |
| buttons | T1...Tn | | user tag buttons |

- **Colour buttons.** Each opens a colour picker with alpha. Right-click gives a secondary action, such as picking from the video or a recent colour.
- **Alignment choice.** The 9 entries run "Bottom-left (an1)" to "Top-right (an9)". The default shown is an2.
- **User tag buttons:**
  - Up to 20. The count is set with "Change number of buttons" or "Manage tag buttons" (Zarządzaj przyciskami tagów).
  - Right-clicking a button opens a small config dialog with these fields:
    - Enter ASS tag (Wpisz tag ASS).
    - Button name (Nazwa przycisku).
    - Mode: Tag inserted in place of cursor / Insert tag at text beginning / Plain text.
    - Save tag / Cancel.

### 9.3 Info row

- **"Line: N"** (Linia: N).
- **"Wraps: a/b"** (Łamania). Shows the characters per visual line, or the total. It turns red when there are more than 2 lines or more than 43 characters per line. Tooltip: about 43 characters per line and at most 2 lines.
- **"Characters per second: N<=15"** (Znaki na sekundę). Turns red when above 15.
- **"Translator mode"** checkbox (section 5).
- Radios **Time / Frames** (Czas / Klatki). They switch the Start/End/Duration fields between time and frame numbers.

### 9.4 Text editor

- **Syntax colouring** uses theme roles `EDITOR_*`: tag names, tag values, braces, operators, `\N`, drawing code, spelling errors and the selection.
- Typing `\` opens a **tag autocomplete list**.
- **Context menu:**
  - Copy, Cut, Paste.
  - Dictionary lookups for the selected word.
  - Spell-checker language choice.
  - Add word to dictionary / Remove word.
  - "Show status bar" (check). The status bar shows the cursor position and the tag under the cursor.
  - "Automatically change quotes" (check).
- **Editing hotkeys:**
  - Enter commits and goes to the next line. Ctrl+Enter commits and stays.
  - Shift+Enter splits the line at the cursor.
  - Ctrl+, and Ctrl+. insert the start and end difference (time offsets relative to the video).
  - Shift+wheel over a number changes the number.

### 9.5 Fields row

| Field | Tooltip / notes |
|---|---|
| Comment (check) | Sets the line as a comment. Comments are not shown. |
| Layer (spin) | Higher layers are drawn on top. |
| Start / End / Duration (time fields) | The wheel or right-drag adjusts the value. Typing is in overwrite mode. |
| Style (combo) | Followed by an **Edit** button for quick editing of the current style. |
| Actor (editable combo) | |
| MarginL / MarginR / MarginV | |
| Effect (editable combo) | |

When the edit box is narrower than about 850 px, the row wraps. Comment, Actor, the margins and Effect move to a second row. For non-ASS formats, the ASS-only controls are disabled: style, actor, margins, effect, layer and most of the tag bar.

---

## 10. Default shortcuts

Hotkey contexts: **Global**, **Grid** (subtitles), **Editbox**, **Video**, **Audio**. Each context can be remapped in Settings > Hotkeys, or with Shift+click on a menu item or toolbar button.

### 10.1 Global

| Key | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+PgDn / Ctrl+PgUp | Next / previous tab |
| Ctrl+O | Open subtitles |
| Ctrl+Shift+O | Open video |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save as |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+Shift+H | History |
| Ctrl+F | Find |
| Ctrl+H | Find and replace |
| F3 | Find next |
| Ctrl+I | Shift times panel |
| Ctrl+M | Style manager |
| Ctrl+E | Enable/disable editor |
| F9 | Convert to ASS |
| F8 | Convert to SRT |
| F10 | Convert to MDVD |
| F11 | Convert to MPL2 |
| Ctrl+F12 | Convert to TMP |
| Alt+Del | Delete text |
| Shift+Del | Delete lines |
| Ctrl+Left | Set start time from video |
| Ctrl+Right | Set end time from video |
| Alt+Space | Play / Pause |
| Left / Right | Previous / next frame |
| Ctrl+Up / Ctrl+Down | Previous / next line |
| F4 | Join with previous line |
| F5 | Join with next line |
| Shift+Left | Snap start to keyframe |
| Shift+Right | Snap end to keyframe |
| F2 | Select line from video time |
| Ctrl+Shift+Z | Turn off video zoom |
| F1 | Help |
| Alt+F4 | Exit |

### 10.2 Grid

| Key | Action |
|---|---|
| Ctrl+D | Duplicate lines |
| Ctrl+Shift+C | Copy columns |
| Ctrl+Shift+V | Paste columns |
| Ctrl+Q | Show subtitle preview |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / Cut / Paste (fixed) |
| Ctrl+A | Select all |
| Alt+Up / Alt+Down | Move rows |
| Enter | Focus the edit box |

### 10.3 Editbox

| Key | Action |
|---|---|
| Ctrl+B | Bold |
| Ctrl+I | Italic |
| Enter / Numpad Enter | Commit and go to the next line |
| Ctrl+Enter | Commit |
| Shift+Enter | Split the line at the cursor |
| Ctrl+, | Start difference |
| Ctrl+. | End difference |
| Ctrl+D | Next doubtful line (TL) |
| Ctrl+R | Next untranslated line (TL) |
| Alt+Down | Set doubtful and go to the next line (TL) |

### 10.4 Video

| Key | Action |
|---|---|
| Space | Play / Pause |
| L | +5 s |
| ; | -5 s |
| Up / Down | +1 min / -1 min |
| . / , | Next / previous file in the folder |
| Numpad . / Numpad 0 | Volume up / down |
| M / N | Next / previous chapter |
| F | Fullscreen |
| Esc | Leave fullscreen |
| B | Boss key: leave fullscreen and minimise |
| W/S/A/D (+Shift) | Nudge the visual-tool point |

### 10.5 Audio (two default keys for most actions)

| Action | Keys |
|---|---|
| Commit | Enter, G |
| Previous line | Left, Z |
| Next line | Right, X |
| Play (selection / syllable) | Down, S |
| Play line | Up, R |
| Stop | H |
| Go to selection | B |
| Scroll left / right | A / F |
| Play before / after mark | Numpad 0 / Numpad . |
| Play first 500 ms | E |
| Play last 500 ms | D |
| Play 500 ms before | Q |
| Play 500 ms after | W |
| Play to end | T |
| Add lead-in | C |
| Add lead-out | V |

---

## 11. Other dialogs

### 11.1 Find / Find and replace (one dialog, three tabs)

The tabs are **Find**, **Find and replace** and **Find in subtitles**. The third searches files in folders.

**Fields:**

- "Search text" (Szukany tekst) and "Replace with". Both are combos that remember recent entries.
- On the folder tab: Filters (a file mask) and Catalog (a folder), plus a browse button.

**Checkboxes:**

- Match case
- Regular expressions
- Beginning of text
- End of text
- Include comments
- Skip tags
- Skip text
- Search in subfolders and Search in hidden folders (folder tab only)

**Groups:**

- "In field": Text / Styles / Actor / Effect.
- "Lines": All / Selected / From selected, plus a style picker.

**Buttons:**

- Find
- Find in all open subtitles
- Find all in current subtitles
- Replace next
- Replace all
- Replace in all open subtitles
- Close

**Search results window.** This is a list grouped by file or tab with check boxes. It has Check all / Uncheck all, and Replace for the checked results. Double-clicking a result jumps to the line.

### 11.2 Select lines

- Mode **Select** with **With** or **Without**, plus a **Find** text field.
- Checks: Match case, Regular expressions.
- Field: Text / Styles / Actor / Effect / Start time / End time.
- Line types: Dialogue / Comments.
- Selection: Add to selection / Deselect.
- **Action** after selecting: Do nothing / Copy / Cut / Move to beginning / Move to end / Set as comment / Delete.
- Check "Select in all tabs".
- Buttons: Select (OK), Apply, Cancel.

### 11.3 ASS file properties

- **Info:** Title, Author, Translator, Proofreading (Korekta), Timer (Timing), Editing (Edycja).
- **Resolution:** Width x Height, with a "From video" button. There is also a Layout resolution.
- **YCbCr matrix:** None / TV.601 / PC.601 / TV.709 / PC.709 / TV.FCC / PC.FCC / TV.240M / PC.240M.
- **Wrap style:** 0 to 3, each described.
- **Colliding lines:** Normal / Reversed.
- **"Scale border and shadow"** (check).
- Buttons: OK, Cancel.

### 11.4 Style manager (Ctrl+M)

```
+-----------------------------------+----------------------------------+
| Catalog: [name v] [New] [Delete]  |                                  |
| Styles stored ("Magazyn stylów")  |  STYLE EDITOR (docked; can be    |
| [list]            [New]           |  detached as a separate window)  |
|  ^ v (move)       [Copy]          |                                  |
|                   [Edit]          |                                  |
|                   [Load]          |                                  |
|                   [Delete]        |                                  |
|                   [Sort]          |                                  |
| [Add to storage ^] [Add to ASS v] [Add to all open ASS files]        |
| Styles in ASS file                |                                  |
| [list]            [New][Copy][Edit][Load][Delete][Sort][Clear]        |
|  ^ v (move)                       |                                  |
| [Close]  [ ] Detach editing window|                                  |
+-----------------------------------+----------------------------------+
```

- **Storage catalogues** are named sets of styles kept outside any file. The last one used is recorded in the file as `Last Style Storage`.
- Double-clicking a style opens it in the editor.
- Multi-selection is supported for copying between the lists.

### 11.5 Style editor

- **Name** field.
- **Font:** a font combo with a filter field, size, and Bold / Italic / Underline / Strikeout checks.
- **Colours:** Primary, Secondary, Outline, Shadow. Each has a colour button and an alpha spin.
- **Geometry:** Border (outline width), Shadow, Scale X, Scale Y, Angle, Spacing.
- **"Opaque box"** check (BorderStyle 3).
- **Margins:** Left, Right, Vertical.
- **Alignment:** a 3x3 radio grid.
- **Encoding** list.
- **Live preview** at the bottom, using the subtitle renderer. The background colour or checkerboard can be changed.
- Buttons: OK, Cancel, Apply. Fonts that are not installed are flagged.

### 11.6 Other tools

- **Font collector** (Kolekcjoner czcionek). Copies the fonts used to a folder or attaches them to an MKV. Shows a log of fonts that were found and missing.
- **Resample subtitles** (Zmień rozdzielczość napisów). Takes the source and target PlayRes, with an option to take the target from the video, and offers to change the margins, scale and border.
- **Spell checker** (Sprawdź poprawność pisowni). Shows the misspelled word, a list of suggestions, Replace / Replace all, Ignore / Ignore all, Add to dictionary, and moves to the next error. The language is chosen in the editor context menu.
- **Multireplacer.** Batch replace rules.
- **Open auto save.** A list of autosave files with dates.
- **Remove temporary files.** Cleans up the autosave and index caches.
- **Automation hotkeys window.** Maps hotkeys to macros.
- **History** (Ctrl+Shift+H). A list of undo steps, each with a description. Clicking a step jumps to that state.
- **Subtitle comparison and preview.** See 1.2 and 4.2.

### 11.7 Settings (Ustawienia)

A tree on the left and pages on the right. The tree:

- Editor
  - Conversion
  - Advanced
- Video
- Audio
  - Advanced
- Themes (colour editor, using the theme roles listed in Appendix B)
- Hotkeys (a list per context, editable by double-click)
- File associations
- Subtitle properties (defaults for new ASS files)

---

## Appendix A. Config file facts (local install, read-only)

`D:\Programy\Kainote\Config\Config.txt` is a `KEY=value` text file. Notable keys:

| Key | Value / meaning |
|---|---|
| `PROGRAM_LANGUAGE` | `pl` |
| `PROGRAM_THEME` | `DarkSentro` (the built-in themes are DarkSentro and LightSentro) |
| `TOOLBAR_IDS` | the ordered list of command IDs on the toolbar (see 3) |
| `TOOLBAR_ALIGNMENT` | 0 left, 1 top, 2 right, 3 bottom |
| `GRID_FONT` | Tahoma 10 |
| `GRID_TAGS_SWAP_CHARACTER` | `☀` |
| `VIDEO_ZOOM_PERCENT` | 200 |
| `VIDEO_WINDOW_SIZE` | stored video panel size |
| `EDITOR_ON` | player-mode state |
| `AUDIO_BOX_HEIGHT` | 170 |
| `SHIFT_TIMES_ON` | whether the shift panel is shown |
| `SHIFT_TIMES_*` | other shift panel settings |
| `POSTPROCESSOR_*` | post processor settings |

`AudioConfig.txt` holds the audio settings (see 7.6). `LastSession.txt` holds the session (see 1.6).

## Appendix B. Theme colour roles (from `Themes\*.txt`, as a checklist of what must be themable)

- **Window and controls:**
  - `WINDOW_BACKGROUND`, `WINDOW_TEXT`, `WINDOW_WARNING_ELEMENTS`
  - `MENUBAR_*`, `MENU_*`
  - `BUTTON_*` (normal, hover, pushed, border, inactive)
  - `TOGGLE_BUTTON_*`
  - `SLIDER_*`
  - `SCROLLBAR_*`
  - `STATICLINE_*`, `STATUSBAR_*`
  - `TABS_*` (active/inactive tab, text, border, close button)
- **Grid:** `GRID_TEXT`, `GRID_BACKGROUND`, `GRID_DIALOGUE`, `GRID_COMMENT`, `GRID_SELECTION`, `GRID_LINE_VISIBLE_ON_VIDEO`, `GRID_COLLISIONS`, `GRID_LINES`, `GRID_ACTIVE_LINE`, `GRID_HEADER`, `GRID_HEADER_TEXT`, `GRID_LABEL_NORMAL`, `GRID_LABEL_MODIFIED`, `GRID_LABEL_SAVED`, `GRID_LABEL_DOUBTFUL`, `GRID_SPELLCHECKER`, `GRID_COMPARISON_*`
- **Edit box:** `EDITOR_TEXT`, `EDITOR_TAG_NAMES`, `EDITOR_TAG_VALUES`, `EDITOR_CURLY_BRACES`, `EDITOR_TAG_OPERATORS`, `EDITOR_TEMPLATE_*`, `EDITOR_PHRASE_SEARCH`, `EDITOR_BRACES_BACKGROUND`, `EDITOR_BACKGROUND`, `EDITOR_SELECTION`, `EDITOR_BORDER`, `EDITOR_SPELLCHECKER`
- **Audio:**
  - `AUDIO_BACKGROUND`, `AUDIO_LINE_BOUNDARY_START` (red), `AUDIO_LINE_BOUNDARY_END` (orange), `AUDIO_LINE_BOUNDARY_MARK` (magenta), `AUDIO_LINE_BOUNDARY_INACTIVE_LINE`, `AUDIO_PLAY_CURSOR`
  - `AUDIO_SECONDS_BOUNDARIES`, `AUDIO_KEYFRAMES` (green), `AUDIO_SYLLABLE_BOUNDARIES` (yellow), `AUDIO_SYLLABLE_TEXT`
  - `AUDIO_SELECTION_BACKGROUND*`, `AUDIO_WAVEFORM*`, `AUDIO_SPECTRUM_*`
- **Video:** `VIDEO_*` (control panel background, time field, warning).

FS Editor should define its **own** theme format with equivalent roles, and should not reuse Kainote theme files.

## Appendix C. Implementation checklist for FS Editor (Qt mapping)

| Kainote element | Qt 6 Widgets approach |
|---|---|
| custom menu bar | `QMenuBar` + QSS; Shift+click on a `QAction` opens the hotkey editor (event filter on `QMenu`) |
| `KaiToolbar` (vertical, 4 sides, customisable) | `QToolBar` in a `Qt::ToolBarArea`, plus a trailing `QToolButton` with a checkable-action popup; persist the command IDs |
| bottom tab bar with "+", drag, middle-close, split view | `QTabBar` with `setShape(QTabBar::RoundedSouth)` + a corner "+" button; split via a `QSplitter` holding two tab contents |
| tab content | `QSplitter(Vertical)` [ `QSplitter(Horizontal)`[Video, `QSplitter(Vertical)`[Audio, EditBox]], `QSplitter(Horizontal)`[Grid, ShiftPanel] ] |
| grid | `QTableView` + custom model/delegate (number-cell state colours, collision text colour, filter "+" strip, tree rows) or a custom-painted `QAbstractScrollArea` for speed |
| edit box | `QTextEdit`/`QPlainTextEdit` + `QSyntaxHighlighter`, `QCompleter` for tags; the fields row in a flow layout that wraps below 850 px |
| audio box | custom `QWidget` paint + `QScrollBar` + 3 vertical `QSlider`s + a `QToolBar` of 23 actions |
| shift panel | `QScrollArea` with `QGroupBox`es; a header button flips a `QStackedWidget` |
| status bar | `QStatusBar` with 8 permanent `QLabel`s + a message area |
| TL storage | Rust core: parse and write the `TLMode*` keys, the TL-style original+translation pairs, the `\fD` Effect marker and the Actor-prefix markers |
