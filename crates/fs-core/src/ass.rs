//! ASS/SSA document: lossless parser and writer.
//!
//! Every piece of the source text is kept: lines outside known structures are
//! stored verbatim, and every parsed entry remembers the line it came from.
//! When writing, an entry whose parsed value still equals its original line is
//! emitted verbatim; a modified entry is re-serialised field by field, reusing
//! the original spelling of every field whose value did not change.

use std::sync::Arc;

use crate::model::{
    AssVersion, Event, EventField, EventKind, Style, StyleField, split_fields, split_key,
};
use crate::text::{LineEnding, RawLine, dominant_ending, split_lines};

/// Stable identifier of an event within a document (survives reordering and
/// undo/redo; not saved to disk).
pub type EventId = u64;

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

/// A `Key: value` line of `[Script Info]` / `[Aegisub Project Garbage]`.
#[derive(Debug, Clone, PartialEq)]
pub struct InfoEntry {
    /// Comments/blank/unparseable lines preceding this entry.
    pub leading: Vec<RawLine>,
    pub key: String,
    pub value: String,
    pub orig: Option<Box<str>>,
    pub ending: LineEnding,
}

#[derive(Debug, Clone, PartialEq)]
pub struct StyleEntry {
    pub leading: Vec<RawLine>,
    pub style: Style,
    pub orig: Option<Box<str>>,
    pub ending: LineEnding,
}

// Eq lets `Arc<EventEntry>` compare by pointer first (std specialisation)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EventEntry {
    pub id: EventId,
    pub leading: Vec<RawLine>,
    pub event: Event,
    pub orig: Option<Box<str>>,
    pub ending: LineEnding,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FormatLine<F> {
    pub fields: Vec<F>,
    pub orig: Option<Box<str>>,
    pub ending: LineEnding,
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct InfoSection {
    pub entries: Vec<InfoEntry>,
    pub trailing: Vec<RawLine>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct StyleSection {
    pub version: AssVersion,
    /// Lines between the header and the `Format:` line.
    pub pre_format: Vec<RawLine>,
    pub format: Option<FormatLine<StyleField>>,
    pub entries: Vec<Arc<StyleEntry>>,
    pub trailing: Vec<RawLine>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EventSection {
    pub pre_format: Vec<RawLine>,
    pub format: Option<FormatLine<EventField>>,
    pub entries: Vec<Arc<EventEntry>>,
    pub trailing: Vec<RawLine>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum SectionBody {
    /// `[Script Info]`, `[Aegisub Project Garbage]`.
    Info(InfoSection),
    Styles(StyleSection),
    Events(EventSection),
    /// `[Fonts]`, `[Graphics]`, `[Aegisub Extradata]`, unknown or duplicate sections.
    Raw(Vec<RawLine>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SectionKind {
    ScriptInfo,
    ProjectGarbage,
    Styles,
    Events,
    Fonts,
    Graphics,
    Extradata,
    Other,
}

impl SectionKind {
    pub fn from_header(h: &str) -> (SectionKind, Option<AssVersion>) {
        let n = h
            .trim()
            .trim_start_matches('[')
            .trim_end_matches(']')
            .trim()
            .to_ascii_lowercase();
        match n.as_str() {
            "script info" => (SectionKind::ScriptInfo, None),
            "aegisub project garbage" => (SectionKind::ProjectGarbage, None),
            "v4+ styles" => (SectionKind::Styles, Some(AssVersion::Ass)),
            "v4 styles" => (SectionKind::Styles, Some(AssVersion::Ssa)),
            "v4++ styles" => (SectionKind::Styles, Some(AssVersion::AssPlusPlus)),
            "events" => (SectionKind::Events, None),
            "fonts" => (SectionKind::Fonts, None),
            "graphics" => (SectionKind::Graphics, None),
            "aegisub extradata" => (SectionKind::Extradata, None),
            _ => (SectionKind::Other, None),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Section {
    pub kind: SectionKind,
    pub header: RawLine,
    pub body: SectionBody,
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct ParseWarning {
    /// 1-based line number.
    pub line: usize,
    pub message: String,
}

#[derive(Debug, Clone)]
pub struct Document {
    /// Whether the source started with a byte-order mark.
    pub bom: bool,
    /// Lines before the first section header.
    pub preamble: Vec<RawLine>,
    pub sections: Vec<Arc<Section>>,
    /// Line ending used for newly created lines.
    pub default_ending: LineEnding,
    next_id: EventId,
}

// Sections are compared by pointer first: an edit copies only the sections it
// touches, so "did this edit change anything" skips the untouched ones.
impl PartialEq for Document {
    fn eq(&self, o: &Self) -> bool {
        self.bom == o.bom
            && self.preamble == o.preamble
            && self.default_ending == o.default_ending
            && self.next_id == o.next_id
            && self.sections.len() == o.sections.len()
            && (self.sections.iter())
                .zip(&o.sections)
                .all(|(a, b)| Arc::ptr_eq(a, b) || a == b)
    }
}

impl Default for Document {
    fn default() -> Self {
        Document::new()
    }
}

impl Document {
    /// An empty document with the standard three sections, like Aegisub's
    /// "New subtitles".
    pub fn new() -> Self {
        let le = LineEnding::default_for_new();
        let mut doc = Document {
            bom: true,
            preamble: Vec::new(),
            sections: Vec::new(),
            default_ending: le,
            next_id: 1,
        };
        doc.sections.push(Arc::new(Section {
            kind: SectionKind::ScriptInfo,
            header: RawLine::new("[Script Info]", le),
            body: SectionBody::Info(InfoSection {
                entries: Vec::new(),
                trailing: Vec::new(),
            }),
        }));
        doc.set_info("ScriptType", "v4.00+");
        doc.set_info("WrapStyle", "0");
        doc.set_info("ScaledBorderAndShadow", "yes");
        doc.set_info("YCbCr Matrix", "None");
        doc.set_info("PlayResX", "1920");
        doc.set_info("PlayResY", "1080");
        doc.ensure_styles();
        doc.ensure_events();
        doc.add_style(Style::default());
        doc.push_event(Event::dialogue(
            crate::time::Time(0),
            crate::time::Time(5000),
            "Default",
            "",
        ));
        doc
    }

    /// An empty document with no sections at all.
    pub fn empty() -> Self {
        Document {
            bom: false,
            preamble: Vec::new(),
            sections: Vec::new(),
            default_ending: LineEnding::default_for_new(),
            next_id: 1,
        }
    }

    pub fn alloc_id(&mut self) -> EventId {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    // ---- parsing -------------------------------------------------------

    pub fn parse(src: &str) -> Document {
        Self::parse_with_warnings(src).0
    }

    pub fn parse_with_warnings(src: &str) -> (Document, Vec<ParseWarning>) {
        Parser::default().run(src)
    }

    // ---- writing -------------------------------------------------------

    /// Serialise to text (without BOM).
    pub fn to_ass_string(&self) -> String {
        let mut w = Writer {
            out: String::with_capacity(64 * 1024),
            default_ending: self.default_ending,
            last_unterminated: false,
        };
        w.document(self);
        w.out
    }

    /// Serialise to UTF-8 bytes, with BOM if the source had one.
    pub fn to_utf8_bytes(&self) -> Vec<u8> {
        let s = self.to_ass_string();
        let mut v = Vec::with_capacity(s.len() + 3);
        if self.bom {
            v.extend_from_slice(b"\xEF\xBB\xBF");
        }
        v.extend_from_slice(s.as_bytes());
        v
    }

    // ---- section access ------------------------------------------------

    pub fn version(&self) -> AssVersion {
        if let Some(s) = self.styles_section() {
            return s.version;
        }
        match self
            .info("ScriptType")
            .map(|s| s.trim().to_ascii_lowercase())
        {
            Some(t) if t == "v4.00" => AssVersion::Ssa,
            Some(t) if t == "v4.00++" => AssVersion::AssPlusPlus,
            _ => AssVersion::Ass,
        }
    }

    pub(crate) fn find_section(&self, kind: SectionKind) -> Option<usize> {
        self.sections.iter().position(|s| {
            s.kind == kind
                && match (&s.body, kind) {
                    (
                        SectionBody::Info(_),
                        SectionKind::ScriptInfo | SectionKind::ProjectGarbage,
                    ) => true,
                    (SectionBody::Styles(_), SectionKind::Styles) => true,
                    (SectionBody::Events(_), SectionKind::Events) => true,
                    (SectionBody::Raw(_), _) => !matches!(
                        kind,
                        SectionKind::ScriptInfo
                            | SectionKind::ProjectGarbage
                            | SectionKind::Styles
                            | SectionKind::Events
                    ),
                    _ => false,
                }
        })
    }

    pub fn info_section(&self) -> Option<&InfoSection> {
        self.kv_section(SectionKind::ScriptInfo)
    }

    pub fn kv_section(&self, kind: SectionKind) -> Option<&InfoSection> {
        let i = self.find_section(kind)?;
        match &self.sections[i].body {
            SectionBody::Info(s) => Some(s),
            _ => None,
        }
    }

    fn kv_section_mut(&mut self, kind: SectionKind) -> &mut InfoSection {
        let i = match self.find_section(kind) {
            Some(i) => i,
            None => {
                let header = match kind {
                    SectionKind::ProjectGarbage => "[Aegisub Project Garbage]",
                    _ => "[Script Info]",
                };
                let pos = match kind {
                    SectionKind::ScriptInfo => 0,
                    // garbage goes after script info
                    _ => self
                        .find_section(SectionKind::ScriptInfo)
                        .map(|i| i + 1)
                        .unwrap_or(0),
                };
                self.insert_section_with_spacing(
                    pos,
                    Section {
                        kind,
                        header: RawLine::new(header, self.default_ending),
                        body: SectionBody::Info(InfoSection {
                            entries: Vec::new(),
                            trailing: Vec::new(),
                        }),
                    },
                );
                pos
            }
        };
        match &mut Arc::make_mut(&mut self.sections[i]).body {
            SectionBody::Info(s) => s,
            _ => unreachable!(),
        }
    }

    /// Insert a section and keep the blank-line separation that Aegisub writes
    /// between sections.
    pub(crate) fn insert_section_with_spacing(&mut self, pos: usize, mut sec: Section) {
        let le = self.default_ending;
        if pos > 0 {
            // ensure previous section ends with a blank line
            let prev = Arc::make_mut(&mut self.sections[pos - 1]);
            let trailing = section_trailing_mut(&mut prev.body);
            let ends_blank = match trailing {
                Some(t) => t.last().is_some_and(|l| l.text.trim().is_empty()),
                None => true,
            };
            if !ends_blank && let Some(t) = section_trailing_mut(&mut prev.body) {
                fix_last_ending(t, le);
                t.push(RawLine::new("", le));
            }
        }
        if pos < self.sections.len()
            && let Some(t) = section_trailing_mut(&mut sec.body)
        {
            t.push(RawLine::new("", le));
        }
        self.sections.insert(pos, Arc::new(sec));
    }

    pub fn styles_section(&self) -> Option<&StyleSection> {
        let i = self.find_section(SectionKind::Styles)?;
        match &self.sections[i].body {
            SectionBody::Styles(s) => Some(s),
            _ => None,
        }
    }

    pub fn ensure_styles(&mut self) -> &mut StyleSection {
        let i = match self.find_section(SectionKind::Styles) {
            Some(i) => i,
            None => {
                let version = self.version();
                let pos = self.find_section(SectionKind::Events).unwrap_or_else(|| {
                    self.find_section(SectionKind::ScriptInfo)
                        .map(|i| i + 1)
                        .unwrap_or(0)
                });
                let pos = pos.max(
                    self.find_section(SectionKind::ProjectGarbage)
                        .map(|i| i + 1)
                        .unwrap_or(0)
                        .min(self.sections.len()),
                );
                let le = self.default_ending;
                self.insert_section_with_spacing(
                    pos,
                    Section {
                        kind: SectionKind::Styles,
                        header: RawLine::new(version.styles_header(), le),
                        body: SectionBody::Styles(StyleSection {
                            version,
                            pre_format: Vec::new(),
                            format: Some(FormatLine {
                                fields: StyleField::default_format(version),
                                orig: None,
                                ending: le,
                            }),
                            entries: Vec::new(),
                            trailing: Vec::new(),
                        }),
                    },
                );
                pos
            }
        };
        match &mut Arc::make_mut(&mut self.sections[i]).body {
            SectionBody::Styles(s) => s,
            _ => unreachable!(),
        }
    }

    pub fn events_section(&self) -> Option<&EventSection> {
        let i = self.find_section(SectionKind::Events)?;
        match &self.sections[i].body {
            SectionBody::Events(s) => Some(s),
            _ => None,
        }
    }

    pub fn ensure_events(&mut self) -> &mut EventSection {
        let i = match self.find_section(SectionKind::Events) {
            Some(i) => i,
            None => {
                let version = self.version();
                let pos = self
                    .find_section(SectionKind::Styles)
                    .map(|i| i + 1)
                    .or_else(|| self.find_section(SectionKind::ScriptInfo).map(|i| i + 1))
                    .unwrap_or(self.sections.len());
                let le = self.default_ending;
                self.insert_section_with_spacing(
                    pos,
                    Section {
                        kind: SectionKind::Events,
                        header: RawLine::new("[Events]", le),
                        body: SectionBody::Events(EventSection {
                            pre_format: Vec::new(),
                            format: Some(FormatLine {
                                fields: EventField::default_format(version),
                                orig: None,
                                ending: le,
                            }),
                            entries: Vec::new(),
                            trailing: Vec::new(),
                        }),
                    },
                );
                pos
            }
        };
        match &mut Arc::make_mut(&mut self.sections[i]).body {
            SectionBody::Events(s) => s,
            _ => unreachable!(),
        }
    }

    // ---- script info ---------------------------------------------------

    /// Value of a `[Script Info]` key (case-insensitive, first match).
    pub fn info(&self, key: &str) -> Option<&str> {
        self.kv_get(SectionKind::ScriptInfo, key)
    }

    pub fn kv_get(&self, kind: SectionKind, key: &str) -> Option<&str> {
        self.kv_section(kind)?
            .entries
            .iter()
            .find(|e| e.key.eq_ignore_ascii_case(key))
            .map(|e| e.value.as_str())
    }

    pub fn set_info(&mut self, key: &str, value: &str) {
        self.kv_set(SectionKind::ScriptInfo, key, value)
    }

    pub fn kv_set(&mut self, kind: SectionKind, key: &str, value: &str) {
        let le = self.default_ending;
        let sec = self.kv_section_mut(kind);
        if let Some(e) = sec
            .entries
            .iter_mut()
            .find(|e| e.key.eq_ignore_ascii_case(key))
        {
            e.value = value.to_string();
        } else {
            if let Some(last) = sec.entries.last_mut()
                && last.ending == LineEnding::None
            {
                last.ending = le;
            }
            sec.entries.push(InfoEntry {
                leading: Vec::new(),
                key: key.to_string(),
                value: value.to_string(),
                orig: None,
                ending: le,
            });
        }
    }

    pub fn remove_info(&mut self, key: &str) -> bool {
        self.kv_remove(SectionKind::ScriptInfo, key)
    }

    pub fn kv_remove(&mut self, kind: SectionKind, key: &str) -> bool {
        if self.find_section(kind).is_none() {
            return false;
        }
        let sec = self.kv_section_mut(kind);
        let before = sec.entries.len();
        sec.entries.retain(|e| !e.key.eq_ignore_ascii_case(key));
        before != sec.entries.len()
    }

    /// `PlayResX`/`PlayResY` with the defaults libass applies when they are
    /// missing (one is derived from the other, or 384×288).
    pub fn play_res(&self) -> (i32, i32) {
        let x = self
            .info("PlayResX")
            .and_then(|v| v.trim().parse::<i32>().ok())
            .filter(|&v| v > 0);
        let y = self
            .info("PlayResY")
            .and_then(|v| v.trim().parse::<i32>().ok())
            .filter(|&v| v > 0);
        match (x, y) {
            (Some(x), Some(y)) => (x, y),
            (None, None) => (384, 288),
            (Some(x), None) => (
                x,
                if x == 1280 {
                    1024
                } else {
                    (x as i64 * 3 / 4) as i32
                },
            ),
            (None, Some(y)) => (
                if y == 1024 {
                    1280
                } else {
                    (y as i64 * 4 / 3) as i32
                },
                y,
            ),
        }
    }

    // ---- styles --------------------------------------------------------

    pub fn styles(&self) -> impl Iterator<Item = &Style> {
        self.styles_section()
            .into_iter()
            .flat_map(|s| s.entries.iter().map(|e| &e.style))
    }

    pub fn style_count(&self) -> usize {
        self.styles_section().map_or(0, |s| s.entries.len())
    }

    /// Style by name. Like VSFilter/libass, the *last* style with a given
    /// name wins and lookup is case-sensitive, with `*Default` fallback
    /// handled by the renderer, not here.
    pub fn style(&self, name: &str) -> Option<&Style> {
        let name = name.trim_start_matches('*');
        self.styles_section()?
            .entries
            .iter()
            .rev()
            .map(|e| &e.style)
            .find(|s| s.name == name)
    }

    pub fn style_index(&self, name: &str) -> Option<usize> {
        self.styles_section()?
            .entries
            .iter()
            .rposition(|e| e.style.name == name)
    }

    pub fn add_style(&mut self, style: Style) -> usize {
        let le = self.default_ending;
        let sec = self.ensure_styles();
        if let Some(last) = sec.entries.last_mut()
            && last.ending == LineEnding::None
        {
            Arc::make_mut(last).ending = le;
        }
        sec.entries.push(Arc::new(StyleEntry {
            leading: Vec::new(),
            style,
            orig: None,
            ending: le,
        }));
        sec.entries.len() - 1
    }

    pub fn style_mut(&mut self, idx: usize) -> Option<&mut Style> {
        let sec = self.ensure_styles();
        sec.entries
            .get_mut(idx)
            .map(|e| &mut Arc::make_mut(e).style)
    }

    pub fn remove_style(&mut self, idx: usize) -> Option<Style> {
        let sec = self.ensure_styles();
        if idx >= sec.entries.len() {
            return None;
        }
        let e = sec.entries.remove(idx);
        let e = Arc::unwrap_or_clone(e);
        reattach_leading(
            e.leading,
            sec.entries
                .get_mut(idx)
                .map(|n| &mut Arc::make_mut(n).leading),
            &mut sec.trailing,
        );
        Some(e.style)
    }

    /// Put the styles in `order` (old indices); comments above a style move
    /// with it. False, and nothing changes, unless `order` is a permutation.
    pub fn reorder_styles(&mut self, order: &[usize]) -> bool {
        let le = self.default_ending;
        let sec = self.ensure_styles();
        let n = sec.entries.len();
        let mut seen = vec![false; n];
        if order.len() != n
            || order
                .iter()
                .any(|&i| i >= n || std::mem::replace(&mut seen[i], true))
        {
            return false;
        }
        let eof_without_newline = sec
            .entries
            .last()
            .is_some_and(|e| e.ending == LineEnding::None);
        let mut entries: Vec<_> = order.iter().map(|&i| sec.entries[i].clone()).collect();
        for e in &mut entries {
            if e.ending == LineEnding::None {
                Arc::make_mut(e).ending = le;
            }
        }
        if eof_without_newline && let Some(last) = entries.last_mut() {
            Arc::make_mut(last).ending = LineEnding::None;
        }
        sec.entries = entries;
        true
    }

    // ---- events --------------------------------------------------------

    pub fn events(&self) -> &[Arc<EventEntry>] {
        self.events_section().map_or(&[], |s| &s.entries)
    }

    pub fn event_count(&self) -> usize {
        self.events().len()
    }

    pub fn event(&self, idx: usize) -> Option<&Event> {
        self.events().get(idx).map(|e| &e.event)
    }

    pub fn event_id(&self, idx: usize) -> Option<EventId> {
        self.events().get(idx).map(|e| e.id)
    }

    pub fn index_of_id(&self, id: EventId) -> Option<usize> {
        self.events().iter().position(|e| e.id == id)
    }

    pub fn event_mut(&mut self, idx: usize) -> Option<&mut Event> {
        self.ensure_events()
            .entries
            .get_mut(idx)
            .map(|e| &mut Arc::make_mut(e).event)
    }

    pub fn push_event(&mut self, ev: Event) -> usize {
        let n = self.event_count();
        self.insert_event(n, ev);
        n
    }

    pub fn insert_event(&mut self, idx: usize, event: Event) -> EventId {
        let id = self.alloc_id();
        let le = self.default_ending;
        let sec = self.ensure_events();
        let idx = idx.min(sec.entries.len());
        if idx == sec.entries.len()
            && let Some(last) = sec.entries.last_mut()
            && last.ending == LineEnding::None
        {
            Arc::make_mut(last).ending = le;
        }
        sec.entries.insert(
            idx,
            Arc::new(EventEntry {
                id,
                leading: Vec::new(),
                event,
                orig: None,
                ending: le,
            }),
        );
        id
    }

    pub fn remove_event(&mut self, idx: usize) -> Option<Event> {
        let sec = self.ensure_events();
        if idx >= sec.entries.len() {
            return None;
        }
        let e = Arc::unwrap_or_clone(sec.entries.remove(idx));
        let was_last_unterminated = e.ending == LineEnding::None;
        reattach_leading(
            e.leading,
            sec.entries
                .get_mut(idx)
                .map(|n| &mut Arc::make_mut(n).leading),
            &mut sec.trailing,
        );
        if was_last_unterminated
            && idx == sec.entries.len()
            && sec.trailing.is_empty()
            && let Some(last) = sec.entries.last_mut()
        {
            Arc::make_mut(last).ending = LineEnding::None;
        }
        Some(e.event)
    }

    /// [`insert_event`](Self::insert_event) for many events in one pass:
    /// `items` are `(index in the current list, event)` in ascending order.
    /// Returns the new indices.
    pub fn insert_events(&mut self, items: Vec<(usize, Event)>) -> Vec<usize> {
        debug_assert!(items.is_sorted_by_key(|i| i.0));
        let first_id = self.next_id;
        self.next_id += items.len() as EventId;
        let le = self.default_ending;
        let sec = self.ensure_events();
        let n = sec.entries.len();
        if items.last().is_some_and(|i| i.0 >= n)
            && let Some(last) = sec.entries.last_mut()
            && last.ending == LineEnding::None
        {
            Arc::make_mut(last).ending = le;
        }
        let old = std::mem::replace(&mut sec.entries, Vec::with_capacity(n + items.len()));
        let mut items = (first_id..).zip(items).peekable();
        let mut out = Vec::new();
        let mut old = old.into_iter().enumerate();
        loop {
            let next = old.next();
            let at = next.as_ref().map_or(usize::MAX, |(i, _)| *i);
            while let Some((id, (_, event))) = items.next_if(|(_, (i, _))| *i <= at) {
                out.push(sec.entries.len());
                sec.entries.push(Arc::new(EventEntry {
                    id,
                    leading: Vec::new(),
                    event,
                    orig: None,
                    ending: le,
                }));
            }
            match next {
                Some((_, e)) => sec.entries.push(e),
                None => break,
            }
        }
        out
    }

    /// [`remove_event`](Self::remove_event) for many events in one pass
    /// (`rows` ascending, no duplicates). Returns how many were removed.
    pub fn remove_events(&mut self, rows: &[usize]) -> usize {
        debug_assert!(rows.is_sorted() && rows.windows(2).all(|w| w[0] != w[1]));
        let sec = self.ensure_events();
        let n = sec.entries.len();
        let last_unterminated =
            n > 0 && rows.last() == Some(&(n - 1)) && sec.entries[n - 1].ending == LineEnding::None;
        let mut gone = rows.iter().copied().filter(|&r| r < n).peekable();
        let mut removed = 0;
        // comments of removed lines move to the next kept line, as remove_event does
        let mut carry: Vec<RawLine> = Vec::new();
        let old = std::mem::take(&mut sec.entries);
        for (i, e) in old.into_iter().enumerate() {
            if gone.next_if_eq(&i).is_some() {
                removed += 1;
                // copy the comments only: the entry may be shared with an undo state
                carry.extend(
                    e.leading
                        .iter()
                        .filter(|l| !l.text.trim().is_empty())
                        .cloned(),
                );
                continue;
            }
            let mut e = e;
            if !carry.is_empty() {
                let lead = &mut Arc::make_mut(&mut e).leading;
                carry.append(lead);
                *lead = std::mem::take(&mut carry);
            }
            sec.entries.push(e);
        }
        if !carry.is_empty() {
            carry.append(&mut sec.trailing);
            sec.trailing = carry;
        } else if last_unterminated
            && sec.trailing.is_empty()
            && let Some(last) = sec.entries.last_mut()
        {
            Arc::make_mut(last).ending = LineEnding::None;
        }
        removed
    }

    /// Remove all events for which `keep` returns false.
    pub fn retain_events(&mut self, mut keep: impl FnMut(&Event) -> bool) -> usize {
        let gone: Vec<usize> = (self.events().iter().enumerate())
            .filter(|(_, e)| !keep(&e.event))
            .map(|(i, _)| i)
            .collect();
        self.remove_events(&gone)
    }

    /// Mutable access to the raw entry vector (for bulk operations such as
    /// sorting). Callers must not create duplicate ids.
    pub fn event_entries_mut(&mut self) -> &mut Vec<Arc<EventEntry>> {
        &mut self.ensure_events().entries
    }

    /// Clone of an event entry with a fresh id (for duplicate/paste).
    pub fn new_entry(&mut self, event: Event) -> Arc<EventEntry> {
        let id = self.alloc_id();
        Arc::new(EventEntry {
            id,
            leading: Vec::new(),
            event,
            orig: None,
            ending: self.default_ending,
        })
    }

    pub fn raw_section(&self, kind: SectionKind) -> Option<&[RawLine]> {
        let i = self.find_section(kind)?;
        match &self.sections[i].body {
            SectionBody::Raw(l) => Some(l),
            _ => None,
        }
    }
}

fn section_trailing_mut(body: &mut SectionBody) -> Option<&mut Vec<RawLine>> {
    match body {
        SectionBody::Info(s) => Some(&mut s.trailing),
        SectionBody::Styles(s) => Some(&mut s.trailing),
        SectionBody::Events(s) => Some(&mut s.trailing),
        SectionBody::Raw(l) => Some(l),
    }
}

pub(crate) fn fix_last_ending(lines: &mut [RawLine], le: LineEnding) {
    if let Some(l) = lines.last_mut()
        && l.ending == LineEnding::None
    {
        l.ending = le;
    }
}

/// Comments attached to a removed entry move to the next entry (or the
/// section trailer) so they are not silently lost.
fn reattach_leading(
    leading: Vec<RawLine>,
    next: Option<&mut Vec<RawLine>>,
    trailing: &mut Vec<RawLine>,
) {
    if leading.is_empty() {
        return;
    }
    let keep: Vec<RawLine> = leading
        .into_iter()
        .filter(|l| !l.text.trim().is_empty())
        .collect();
    if keep.is_empty() {
        return;
    }
    match next {
        Some(n) => {
            let mut v = keep;
            v.append(n);
            *n = v;
        }
        None => {
            let mut v = keep;
            v.append(trailing);
            *trailing = v;
        }
    }
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

#[derive(Default)]
struct Parser {
    warnings: Vec<ParseWarning>,
}

enum Cur {
    None,
    Section(usize),
}

impl Parser {
    fn run(mut self, src: &str) -> (Document, Vec<ParseWarning>) {
        let (bom, src) = match src.strip_prefix('\u{feff}') {
            Some(r) => (true, r),
            None => (false, src),
        };
        let lines = split_lines(src);
        let mut doc = Document::empty();
        doc.bom = bom;
        doc.default_ending = dominant_ending(lines.iter().map(|(_, e)| e));
        let mut cur = Cur::None;
        // pending leading lines for the next entry of the current section
        let mut pending: Vec<RawLine> = Vec::new();
        // version guess for events when no styles section precedes them
        let mut version_hint: Option<AssVersion> = None;
        // a later Format line in the current section overrides its first one
        let mut style_fmt: Option<Vec<StyleField>> = None;
        let mut event_fmt: Option<Vec<EventField>> = None;

        for (lineno0, (text, ending)) in lines.iter().enumerate() {
            let lineno = lineno0 + 1;
            let raw = RawLine::new(*text, *ending);
            let trimmed = text.trim();

            // Section header?
            if trimmed.starts_with('[') && trimmed.ends_with(']') && trimmed.len() >= 2 {
                // flush pending to current section trailer
                self.flush_pending(&mut doc, &cur, &mut pending);
                (style_fmt, event_fmt) = (None, None);
                let (mut kind, ver) = SectionKind::from_header(trimmed);
                // Duplicate typed sections are kept verbatim.
                let dup = matches!(
                    kind,
                    SectionKind::ScriptInfo | SectionKind::Styles | SectionKind::Events
                ) && doc
                    .sections
                    .iter()
                    .any(|s| s.kind == kind && !matches!(s.body, SectionBody::Raw(_)));
                if dup {
                    self.warn(lineno, format!("duplicate section {trimmed} kept verbatim"));
                }
                let body = if dup {
                    SectionBody::Raw(Vec::new())
                } else {
                    match kind {
                        SectionKind::ScriptInfo | SectionKind::ProjectGarbage => {
                            SectionBody::Info(InfoSection {
                                entries: Vec::new(),
                                trailing: Vec::new(),
                            })
                        }
                        SectionKind::Styles => {
                            let v = ver.unwrap_or_default();
                            version_hint = Some(v);
                            SectionBody::Styles(StyleSection {
                                version: v,
                                pre_format: Vec::new(),
                                format: None,
                                entries: Vec::new(),
                                trailing: Vec::new(),
                            })
                        }
                        SectionKind::Events => SectionBody::Events(EventSection {
                            pre_format: Vec::new(),
                            format: None,
                            entries: Vec::new(),
                            trailing: Vec::new(),
                        }),
                        _ => SectionBody::Raw(Vec::new()),
                    }
                };
                if dup {
                    kind = SectionKind::Other;
                }
                doc.sections.push(Arc::new(Section {
                    kind,
                    header: raw,
                    body,
                }));
                cur = Cur::Section(doc.sections.len() - 1);
                continue;
            }

            let si = match cur {
                Cur::None => {
                    doc.preamble.push(raw);
                    continue;
                }
                Cur::Section(i) => i,
            };

            // Script type hint from [Script Info] before any styles section.
            let next_id = &mut doc.next_id;
            match &mut Arc::make_mut(&mut doc.sections[si]).body {
                SectionBody::Raw(l) => l.push(raw),
                SectionBody::Info(sec) => {
                    if trimmed.is_empty() || trimmed.starts_with(';') || trimmed.starts_with("!:") {
                        pending.push(raw);
                        continue;
                    }
                    match split_key(text) {
                        Some((key, p)) => {
                            let key = key.trim().to_string();
                            if key.eq_ignore_ascii_case("ScriptType") && version_hint.is_none() {
                                version_hint = match text[p..].trim().to_ascii_lowercase().as_str()
                                {
                                    "v4.00" => Some(AssVersion::Ssa),
                                    "v4.00++" => Some(AssVersion::AssPlusPlus),
                                    _ => Some(AssVersion::Ass),
                                };
                            }
                            sec.entries.push(InfoEntry {
                                leading: std::mem::take(&mut pending),
                                key,
                                value: text[p..].to_string(),
                                orig: Some((*text).into()),
                                ending: *ending,
                            });
                        }
                        None => pending.push(raw),
                    }
                }
                SectionBody::Styles(sec) => {
                    if trimmed.is_empty() || trimmed.starts_with(';') || trimmed.starts_with("!:") {
                        pending.push(raw);
                        continue;
                    }
                    let Some((key, p)) = split_key(text) else {
                        self.warn(lineno, "unparseable line in styles section");
                        pending.push(raw);
                        continue;
                    };
                    let key_l = key.trim().to_ascii_lowercase();
                    if key_l == "format" {
                        let fields: Vec<StyleField> =
                            text[p..].split(',').map(StyleField::from_name).collect();
                        if sec.format.is_none() && sec.entries.is_empty() {
                            sec.pre_format = std::mem::take(&mut pending);
                            sec.format = Some(FormatLine {
                                fields,
                                orig: Some((*text).into()),
                                ending: *ending,
                            });
                        } else {
                            // A later Format line: keep verbatim; the writer
                            // re-detects it to keep field order consistent.
                            style_fmt = Some(fields);
                            pending.push(raw);
                        }
                    } else if key_l == "style" {
                        let default;
                        let fmt: &[StyleField] = match (&style_fmt, &sec.format) {
                            (Some(f), _) => f,
                            (None, Some(f)) => &f.fields,
                            (None, None) => {
                                default = StyleField::default_format(sec.version);
                                &default
                            }
                        };
                        let (fields, complete) = split_fields(&text[p..], fmt.len());
                        if !complete {
                            self.warn(lineno, "style line has fewer fields than Format");
                        }
                        let mut style = Style::default();
                        for (f, v) in fmt.iter().zip(fields) {
                            style.set_field(f, v, sec.version);
                        }
                        sec.entries.push(Arc::new(StyleEntry {
                            leading: std::mem::take(&mut pending),
                            style,
                            orig: Some((*text).into()),
                            ending: *ending,
                        }));
                    } else {
                        self.warn(
                            lineno,
                            format!("unknown line type '{}' in styles section", key.trim()),
                        );
                        pending.push(raw);
                    }
                }
                SectionBody::Events(sec) => {
                    if trimmed.is_empty() || trimmed.starts_with(';') || trimmed.starts_with("!:") {
                        pending.push(raw);
                        continue;
                    }
                    let Some((key, p)) = split_key(text) else {
                        self.warn(lineno, "unparseable line in events section");
                        pending.push(raw);
                        continue;
                    };
                    let key_t = key.trim();
                    if key_t.eq_ignore_ascii_case("format") {
                        let fields: Vec<EventField> =
                            text[p..].split(',').map(EventField::from_name).collect();
                        if sec.format.is_none() && sec.entries.is_empty() {
                            sec.pre_format = std::mem::take(&mut pending);
                            sec.format = Some(FormatLine {
                                fields,
                                orig: Some((*text).into()),
                                ending: *ending,
                            });
                        } else {
                            event_fmt = Some(fields);
                            pending.push(raw);
                        }
                    } else if let Some(kind) = EventKind::from_key(key_t) {
                        let default;
                        let fmt: &[EventField] = match (&event_fmt, &sec.format) {
                            (Some(f), _) => f,
                            (None, Some(f)) => &f.fields,
                            (None, None) => {
                                default =
                                    EventField::default_format(version_hint.unwrap_or_default());
                                &default
                            }
                        };
                        let (fields, complete) = split_fields(&text[p..], fmt.len());
                        if !complete {
                            self.warn(lineno, "event line has fewer fields than Format");
                        }
                        let mut event = Event {
                            kind,
                            ..Default::default()
                        };
                        for (f, v) in fmt.iter().zip(fields) {
                            event.set_field(f, v);
                        }
                        let id = *next_id;
                        *next_id += 1;
                        sec.entries.push(Arc::new(EventEntry {
                            id,
                            leading: std::mem::take(&mut pending),
                            event,
                            orig: Some((*text).into()),
                            ending: *ending,
                        }));
                    } else {
                        self.warn(
                            lineno,
                            format!("unknown line type '{key_t}' in events section"),
                        );
                        pending.push(raw);
                    }
                }
            }
        }
        self.flush_pending(&mut doc, &cur, &mut pending);
        (doc, self.warnings)
    }

    fn flush_pending(&mut self, doc: &mut Document, cur: &Cur, pending: &mut Vec<RawLine>) {
        if pending.is_empty() {
            return;
        }
        let Cur::Section(i) = *cur else {
            doc.preamble.append(pending);
            return;
        };
        match &mut Arc::make_mut(&mut doc.sections[i]).body {
            SectionBody::Info(s) => s.trailing.append(pending),
            SectionBody::Styles(s) => {
                if s.format.is_none() && s.entries.is_empty() {
                    s.pre_format.append(pending)
                } else {
                    s.trailing.append(pending)
                }
            }
            SectionBody::Events(s) => {
                if s.format.is_none() && s.entries.is_empty() {
                    s.pre_format.append(pending)
                } else {
                    s.trailing.append(pending)
                }
            }
            SectionBody::Raw(l) => l.append(pending),
        }
    }

    fn warn(&mut self, line: usize, msg: impl Into<String>) {
        self.warnings.push(ParseWarning {
            line,
            message: msg.into(),
        });
    }
}

/// Detect a `Format:` line among raw lines (used for mid-section format changes).
fn raw_format_line(l: &RawLine) -> Option<&str> {
    let (key, p) = split_key(&l.text)?;
    key.trim()
        .eq_ignore_ascii_case("format")
        .then(|| &l.text[p..])
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct Writer {
    out: String,
    default_ending: LineEnding,
    last_unterminated: bool,
}

impl Writer {
    fn line(&mut self, text: &str, ending: LineEnding) {
        self.out.push_str(text);
        // An unterminated line can only legitimately be the last one; the
        // caller fixes up the final ending afterwards.
        let e = if ending == LineEnding::None {
            self.default_ending
        } else {
            ending
        };
        self.out.push_str(e.as_str());
        self.last_unterminated = ending == LineEnding::None;
    }

    fn raw(&mut self, l: &RawLine) {
        self.line(&l.text, l.ending);
    }

    fn raws(&mut self, ls: &[RawLine]) {
        for l in ls {
            self.raw(l);
        }
    }

    fn document(&mut self, doc: &Document) {
        self.raws(&doc.preamble);
        let version = doc.version();
        for sec in &doc.sections {
            self.raw(&sec.header);
            match &sec.body {
                SectionBody::Raw(l) => self.raws(l),
                SectionBody::Info(s) => {
                    for e in &s.entries {
                        self.raws(&e.leading);
                        self.info_entry(e);
                    }
                    self.raws(&s.trailing);
                }
                SectionBody::Styles(s) => {
                    self.raws(&s.pre_format);
                    let mut fmt = match &s.format {
                        Some(f) => {
                            self.format_line(f.orig.as_deref(), f.ending, || {
                                f.fields
                                    .iter()
                                    .map(|x| x.name(s.version).to_string())
                                    .collect()
                            });
                            f.fields.clone()
                        }
                        None => StyleField::default_format(s.version),
                    };
                    for e in &s.entries {
                        for l in &e.leading {
                            if let Some(f) = raw_format_line(l) {
                                fmt = f.split(',').map(StyleField::from_name).collect();
                            }
                            self.raw(l);
                        }
                        self.style_entry(e, &fmt, s.version);
                    }
                    self.raws(&s.trailing);
                }
                SectionBody::Events(s) => {
                    self.raws(&s.pre_format);
                    let mut fmt = match &s.format {
                        Some(f) => {
                            self.format_line(f.orig.as_deref(), f.ending, || {
                                f.fields.iter().map(|x| x.name().to_string()).collect()
                            });
                            f.fields.clone()
                        }
                        None => EventField::default_format(version),
                    };
                    for e in &s.entries {
                        for l in &e.leading {
                            if let Some(f) = raw_format_line(l) {
                                fmt = f.split(',').map(EventField::from_name).collect();
                            }
                            self.raw(l);
                        }
                        self.event_entry(e, &fmt);
                    }
                    self.raws(&s.trailing);
                }
            }
        }
        if self.last_unterminated {
            let e = self.default_ending.as_str();
            self.out.truncate(self.out.len() - e.len());
        }
    }

    fn format_line(
        &mut self,
        orig: Option<&str>,
        ending: LineEnding,
        names: impl FnOnce() -> Vec<String>,
    ) {
        match orig {
            Some(o) => self.line(o, ending),
            None => {
                let s = format!("Format: {}", names().join(", "));
                self.line(&s, ending);
            }
        }
    }

    fn info_entry(&mut self, e: &InfoEntry) {
        if let Some(o) = e.orig.as_deref()
            && let Some((k, p)) = split_key(o)
            && k.trim() == e.key
            && o[p..] == *e.value
        {
            self.line(o, e.ending);
            return;
        }
        let s = match e
            .orig
            .as_deref()
            .and_then(|o| split_key(o).map(|(k, p)| (k, &o[..p])))
        {
            // keep original key spelling and separator when only the value changed
            Some((k, prefix)) if k.trim() == e.key => format!("{prefix}{}", e.value),
            _ => format!("{}: {}", e.key, e.value),
        };
        self.line(&s, e.ending);
    }

    fn style_entry(&mut self, e: &StyleEntry, fmt: &[StyleField], version: AssVersion) {
        if let Some(o) = e.orig.as_deref() {
            let reparsed = parse_style_line(o, fmt, version);
            if reparsed.as_ref() == Some(&e.style) {
                self.line(o, e.ending);
                return;
            }
        }
        let hint = e
            .orig
            .as_deref()
            .and_then(|o| split_key(o).map(|(_, p)| (&o[..p], split_fields(&o[p..], fmt.len()).0)));
        let mut s = String::with_capacity(128);
        match &hint {
            Some((prefix, _)) => s.push_str(prefix),
            None => s.push_str("Style: "),
        }
        for (i, f) in fmt.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            let new = e.style.get_field(f, version);
            if let Some((_, fields)) = &hint
                && let Some(old) = fields.get(i)
            {
                let mut tmp = Style::default();
                tmp.set_field(f, old, version);
                if tmp.get_field(f, version) == new {
                    s.push_str(old);
                    continue;
                }
            }
            s.push_str(&new);
        }
        self.line(&s, e.ending);
    }

    fn event_entry(&mut self, e: &EventEntry, fmt: &[EventField]) {
        let ev = &e.event;
        // the source line split once: (prefix "Dialogue: ", same kind, fields)
        let orig = e.orig.as_deref().and_then(|o| {
            let (k, p) = split_key(o)?;
            let same_kind = EventKind::from_key(k) == Some(ev.kind);
            Some((o, &o[..p], same_kind, split_fields(&o[p..], fmt.len()).0))
        });
        if let Some((o, _, true, fields)) = &orig
            && fmt.iter().zip(fields).all(|(f, v)| ev.field_matches(f, v))
        {
            self.line(o, e.ending);
            return;
        }
        let mut s = String::with_capacity(64 + ev.text.len());
        match &orig {
            Some((_, prefix, true, _)) => s.push_str(prefix),
            _ => {
                s.push_str(ev.kind.key());
                s.push_str(": ");
            }
        }
        for (i, f) in fmt.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            match orig.as_ref().and_then(|(_, _, _, fields)| fields.get(i)) {
                // an unchanged field keeps its original spelling
                Some(old) if ev.field_matches(f, old) => s.push_str(old),
                _ => s.push_str(&ev.get_field(f)),
            }
        }
        self.line(&s, e.ending);
    }
}

fn parse_style_line(line: &str, fmt: &[StyleField], version: AssVersion) -> Option<Style> {
    let (_, p) = split_key(line)?;
    let (fields, _) = split_fields(&line[p..], fmt.len());
    let mut st = Style::default();
    for (f, v) in fmt.iter().zip(fields) {
        st.set_field(f, v, version);
    }
    Some(st)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reorder_styles_keeps_comments_and_eof() {
        let fmt = "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding";
        let st = |n: &str| {
            format!(
                "Style: {n},Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1"
            )
        };
        let src = format!(
            "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\n{fmt}\n{}\n; about B\n{}\n{}",
            st("A"),
            st("B"),
            st("C")
        );
        let mut d = Document::parse(&src);
        assert!(!d.reorder_styles(&[0, 0, 1]));
        assert!(!d.reorder_styles(&[0, 1]));
        assert!(d.reorder_styles(&[2, 1, 0]));
        let names: Vec<_> = d.styles().map(|s| s.name.as_str()).collect();
        assert_eq!(names, ["C", "B", "A"]);
        let out = d.to_ass_string();
        assert!(
            out.contains(&format!("{}\n; about B\n{}\n{}", st("C"), st("B"), st("A"))),
            "{out}"
        );
        assert!(!out.ends_with('\n'));
    }
}
