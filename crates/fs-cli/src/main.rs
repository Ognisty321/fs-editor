//! `fsed` — FS Editor command line tool.
//!
//! Exposes the editing core for scripting and testing: inspect, validate,
//! convert, shift, sort, filter, replace, resample and translation-mode
//! operations on subtitle files.

use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{Context, Result, bail};
use clap::{Args, Parser, Subcommand, ValueEnum};
use fs_core::history::Editor;
use fs_core::ops::{self, Field, Filter, Rows, ShiftAmount, ShiftWhich, SortKey, TextMatcher};
use fs_core::vfr::{Framerate, TimeType};
use fs_core::{Document, Time, tags, tlmode};
use fs_formats::{Format, LoadOptions, Loaded, SaveOptions};

#[cfg(feature = "lua")]
mod lua;
#[cfg(feature = "media")]
mod media;

#[derive(Parser)]
#[command(
    name = "fsed",
    version,
    about = "FS Editor — subtitle (ASS/SSA/SRT/MPL2/MicroDVD/TMP) command line tool"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Show information about a subtitle file.
    Info {
        file: PathBuf,
        #[command(flatten)]
        load: LoadArgs,
        /// CPS limit used for statistics.
        #[arg(long, default_value_t = 15.0)]
        cps: f64,
    },
    /// Validate a file: parse warnings, missing styles, overlaps, CPS,
    /// negative durations. Exit code 1 if problems were found.
    Check {
        files: Vec<PathBuf>,
        #[command(flatten)]
        load: LoadArgs,
        #[arg(long, default_value_t = 15.0)]
        cps: f64,
        /// Treat overlaps and CPS as errors too (default: warnings).
        #[arg(long)]
        strict: bool,
    },
    /// Verify that loading and saving reproduces each file byte-for-byte.
    Roundtrip { files: Vec<PathBuf> },
    /// Print the lines of a file as a table (like the editor grid).
    Lines {
        file: PathBuf,
        #[command(flatten)]
        load: LoadArgs,
        #[command(flatten)]
        filter: FilterArgs,
        /// Show text without override tags.
        #[arg(long)]
        strip: bool,
    },
    /// Font collector: list the fonts the lines use, find their files and
    /// missing glyphs, optionally copy them. Exit code 1 if any is missing.
    Fonts {
        file: PathBuf,
        #[command(flatten)]
        load: LoadArgs,
        /// Extra folders searched besides the system fonts (e.g. fonts
        /// extracted from the video).
        #[arg(long = "dir")]
        dirs: Vec<PathBuf>,
        /// Copy every found font file into this folder.
        #[arg(long)]
        collect: Option<PathBuf>,
    },
    /// List styles.
    Styles {
        file: PathBuf,
        #[command(flatten)]
        load: LoadArgs,
    },
    /// Convert between formats (format chosen by --to or output extension).
    Convert {
        input: PathBuf,
        #[arg(short, long)]
        output: PathBuf,
        #[arg(long)]
        to: Option<FormatArg>,
        #[command(flatten)]
        load: LoadArgs,
        #[command(flatten)]
        save: SaveArgs,
    },
    /// Shift line times by a duration or a number of frames.
    Shift {
        file: PathBuf,
        /// Offset such as `1.5s`, `-250ms`, `0:00:02.10`.
        #[arg(long, allow_hyphen_values = true, conflicts_with = "frames")]
        by: Option<String>,
        /// Shift by whole frames (requires --fps or --timecodes).
        #[arg(long, allow_hyphen_values = true)]
        frames: Option<i64>,
        #[command(flatten)]
        timing: TimingArgs,
        /// Shift only start times.
        #[arg(long, conflicts_with = "end_only")]
        start_only: bool,
        /// Shift only end times.
        #[arg(long)]
        end_only: bool,
        /// Only lines from this 1-based line number onward.
        #[arg(long)]
        from_line: Option<usize>,
        /// Only lines starting at or after this time.
        #[arg(long)]
        after: Option<String>,
        /// Only lines with these styles (comma separated).
        #[arg(long, value_delimiter = ',')]
        styles: Vec<String>,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Sort lines.
    Sort {
        file: PathBuf,
        #[arg(long, value_enum, default_value_t = SortArg::Start)]
        by: SortArg,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Keep only lines matching a filter (others are deleted).
    Filter {
        file: PathBuf,
        #[command(flatten)]
        filter: FilterArgs,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Find and replace in line text (or another field).
    Replace {
        file: PathBuf,
        #[arg(long)]
        find: String,
        #[arg(long, allow_hyphen_values = true)]
        replace: String,
        #[arg(long)]
        regex: bool,
        #[arg(long, short = 'i')]
        ignore_case: bool,
        /// Only replace in visible text, never inside override tags.
        #[arg(long)]
        skip_tags: bool,
        #[arg(long, value_enum, default_value_t = FieldArg::Text)]
        field: FieldArg,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Change script resolution, scaling everything accordingly.
    Resample {
        file: PathBuf,
        /// Target resolution, e.g. 1920x1080.
        #[arg(long)]
        to: String,
        /// Source resolution (default: PlayResX/Y of the script).
        #[arg(long)]
        from: Option<String>,
        /// Stretch when the aspect ratio changes (default: keep proportions).
        #[arg(long)]
        stretch: bool,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Convert timing from one constant frame rate to another.
    ChangeFps {
        file: PathBuf,
        #[arg(long)]
        from: String,
        #[arg(long)]
        to: String,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Timing post-processor: lead-in/out, link adjacent lines, snap to keyframes.
    Postprocess {
        file: PathBuf,
        #[arg(long)]
        lead_in: Option<String>,
        #[arg(long)]
        lead_out: Option<String>,
        /// Link lines closer than this.
        #[arg(long)]
        adjacent: Option<String>,
        /// Where linked lines meet within the gap (0 = previous end, 1 = next start).
        #[arg(long, default_value_t = 0.5)]
        bias: f64,
        /// Keyframes file for snapping (requires --fps or --timecodes).
        #[arg(long)]
        keyframes: Option<PathBuf>,
        /// Snap distance (applies before/after start and end).
        #[arg(long, default_value = "250ms")]
        snap: String,
        #[command(flatten)]
        timing: TimingArgs,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Translation mode (Kainote compatible) operations.
    Tl {
        #[command(subcommand)]
        cmd: TlCmd,
    },
    /// Frame/time conversions for a frame rate or timecodes file.
    Frame {
        /// Time (e.g. 0:01:02.50) or frame number (prefix with `f`, e.g. f1500).
        value: String,
        #[command(flatten)]
        timing: TimingArgs,
    },
    /// Convert a v1/v2 timecodes file to v2.
    Timecodes {
        input: PathBuf,
        /// Number of frames to write (extrapolated past the end).
        #[arg(long, default_value_t = 0)]
        frames: usize,
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Run an Automation 4 macro or export filter (Lua/MoonScript) on a
    /// file, or list a script's features.
    #[cfg(feature = "lua")]
    Lua(lua::LuaArgs),
    /// Video/audio files: tracks, frames with subtitles, keyframes,
    /// timecodes, audio clips (BestSource + libass).
    #[cfg(feature = "media")]
    Media {
        #[command(subcommand)]
        cmd: media::MediaCmd,
    },
}

#[derive(Subcommand)]
enum TlCmd {
    /// Show translation progress.
    Status { file: PathBuf },
    /// Turn translation mode on.
    Enable {
        file: PathBuf,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Turn it off, keeping translations (originals of translated lines are removed).
    Disable {
        file: PathBuf,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Fill translations from a plain text file (one line per subtitle).
    Paste {
        file: PathBuf,
        text: PathBuf,
        /// First 1-based row to fill.
        #[arg(long, default_value_t = 1)]
        start: usize,
        #[command(flatten)]
        out: OutArgs,
    },
    /// Write original texts, one per line (for external translation).
    ExtractOriginal { file: PathBuf },
}

#[derive(Args, Clone, Default)]
struct LoadArgs {
    /// Force input encoding (e.g. windows-1250).
    #[arg(long)]
    encoding: Option<String>,
    /// Force input format.
    #[arg(long)]
    format: Option<FormatArg>,
    /// Frame rate for MicroDVD input without an FPS header.
    #[arg(long = "input-fps")]
    input_fps: Option<String>,
}

#[derive(Args, Clone, Default)]
struct SaveArgs {
    /// Output encoding (default: UTF-8).
    #[arg(long = "out-encoding")]
    out_encoding: Option<String>,
    /// Write without a byte-order mark.
    #[arg(long)]
    no_bom: bool,
    /// Frame rate for MicroDVD output.
    #[arg(long = "output-fps")]
    output_fps: Option<String>,
}

#[derive(Args, Clone, Default)]
struct OutArgs {
    /// Output file (default: stdout).
    #[arg(short, long, conflicts_with = "in_place")]
    output: Option<PathBuf>,
    /// Overwrite the input file.
    #[arg(long)]
    in_place: bool,
}

#[derive(Args, Clone, Default)]
struct TimingArgs {
    /// Constant frame rate, e.g. 23.976 or 24000/1001.
    #[arg(long)]
    fps: Option<String>,
    /// Timecodes file (v1 or v2) for variable frame rate.
    #[arg(long)]
    timecodes: Option<PathBuf>,
}

#[derive(Args, Clone, Default)]
struct FilterArgs {
    #[arg(long, value_delimiter = ',')]
    style: Vec<String>,
    #[arg(long, value_delimiter = ',')]
    actor: Vec<String>,
    #[arg(long, value_delimiter = ',')]
    effect: Vec<String>,
    /// Regular expression matched against the text.
    #[arg(long)]
    text: Option<String>,
    /// Only comments.
    #[arg(long, conflicts_with = "dialogue")]
    comments: bool,
    /// Only dialogue (no comments).
    #[arg(long)]
    dialogue: bool,
    /// Only lines overlapping another line.
    #[arg(long)]
    overlapping: bool,
    /// Only lines above this CPS.
    #[arg(long)]
    min_cps: Option<f64>,
    #[arg(long)]
    invert: bool,
}

#[derive(Clone, Copy, ValueEnum)]
enum FormatArg {
    Ass,
    Ssa,
    Srt,
    Mpl2,
    Microdvd,
    Tmp,
}

impl From<FormatArg> for Format {
    fn from(f: FormatArg) -> Format {
        match f {
            FormatArg::Ass => Format::Ass,
            FormatArg::Ssa => Format::Ssa,
            FormatArg::Srt => Format::Srt,
            FormatArg::Mpl2 => Format::Mpl2,
            FormatArg::Microdvd => Format::MicroDvd,
            FormatArg::Tmp => Format::Tmp,
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
enum SortArg {
    Start,
    End,
    Style,
    Actor,
    Effect,
    Layer,
    Text,
}

#[derive(Clone, Copy, ValueEnum)]
enum FieldArg {
    Text,
    Style,
    Actor,
    Effect,
}

// ---------------------------------------------------------------------------

fn parse_fps(s: &str) -> Result<Framerate> {
    Framerate::parse(s).with_context(|| format!("invalid frame rate '{s}'"))
}

impl TimingArgs {
    fn framerate(&self) -> Result<Option<Framerate>> {
        if let Some(tc) = &self.timecodes {
            let text =
                std::fs::read_to_string(tc).with_context(|| format!("reading {}", tc.display()))?;
            return Ok(Some(Framerate::parse_timecodes(&text)?));
        }
        self.fps.as_deref().map(parse_fps).transpose()
    }

    fn require(&self) -> Result<Framerate> {
        self.framerate()?
            .context("a frame rate is required: pass --fps or --timecodes")
    }
}

fn load(path: &Path, args: &LoadArgs) -> Result<Loaded> {
    let mut opts = LoadOptions::default();
    if let Some(e) = &args.encoding {
        opts.encoding = Some(
            fs_formats::encoding::by_name(e).with_context(|| format!("unknown encoding '{e}'"))?,
        );
    }
    opts.format = args.format.map(Into::into);
    if let Some(f) = &args.input_fps {
        opts.fps = parse_fps(f)?;
    }
    fs_formats::load_file(path, &opts).with_context(|| format!("loading {}", path.display()))
}

fn load_default(path: &Path) -> Result<Loaded> {
    load(path, &LoadArgs::default())
}

/// Save an edited document in its original format and encoding.
fn write_out(input: &Path, loaded: &Loaded, doc: &Document, out: &OutArgs) -> Result<()> {
    let opts = SaveOptions {
        encoding: loaded.encoding,
        bom: loaded.bom,
        ..Default::default()
    };
    let bytes = fs_formats::save_bytes(doc, loaded.format, &opts);
    match (&out.output, out.in_place) {
        (Some(p), _) => {
            std::fs::write(p, bytes).with_context(|| format!("writing {}", p.display()))?
        }
        (None, true) => {
            std::fs::write(input, bytes).with_context(|| format!("writing {}", input.display()))?
        }
        (None, false) => std::io::stdout().write_all(&bytes)?,
    }
    Ok(())
}

fn build_filter(f: &FilterArgs) -> Result<Filter> {
    Ok(Filter {
        styles: f.style.clone(),
        actors: f.actor.clone(),
        effects: f.effect.clone(),
        text: f
            .text
            .as_deref()
            .map(|t| TextMatcher::new(t, true, true, Field::Text))
            .transpose()?,
        comments: if f.comments {
            Some(true)
        } else if f.dialogue {
            Some(false)
        } else {
            None
        },
        only_overlapping: f.overlapping,
        min_cps: f.min_cps,
        invert: f.invert,
        rows: None,
    })
}

fn parse_res(s: &str) -> Result<(i32, i32)> {
    let (w, h) = s
        .split_once(['x', 'X', '*'])
        .context("resolution must look like 1920x1080")?;
    Ok((w.trim().parse()?, h.trim().parse()?))
}

pub(crate) fn parse_time(s: &str) -> Result<Time> {
    Time::parse_offset(s).with_context(|| format!("invalid time '{s}'"))
}

// ---------------------------------------------------------------------------

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(cli) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("error: {e:#}");
            ExitCode::from(2)
        }
    }
}

fn run(cli: Cli) -> Result<ExitCode> {
    match cli.cmd {
        Cmd::Info {
            file,
            load: la,
            cps,
        } => {
            let l = load(&file, &la)?;
            let d = &l.doc;
            let st = ops::stats(d, cps);
            println!("File:        {}", file.display());
            println!("Format:      {}", l.format.name());
            println!(
                "Encoding:    {}{}",
                l.encoding.name(),
                if l.bom { " (BOM)" } else { "" }
            );
            if let Some(f) = l.declared_fps {
                println!("Declared FPS: {f}");
            }
            if l.format.is_ass() {
                let (x, y) = d.play_res();
                println!("Version:     {}", d.version().script_type());
                println!("PlayRes:     {x}x{y}");
                if let Some(t) = d.info("Title") {
                    println!("Title:       {t}");
                }
                for k in ["Video File", "Audio File", "Keyframes File"] {
                    if let Some(v) = d
                        .info(k)
                        .or_else(|| d.kv_get(fs_core::SectionKind::ProjectGarbage, k))
                    {
                        println!("{:<13}{v}", format!("{k}:").replace(" File:", ":"));
                    }
                }
                let sections: Vec<String> = d
                    .sections
                    .iter()
                    .map(|s| s.header.text.trim().to_string())
                    .collect();
                println!("Sections:    {}", sections.join(" "));
                if let Some(tl) = tlmode::tl_style(d) {
                    let rows = tlmode::rows(d);
                    let done = rows.iter().filter(|r| r.translation.is_some()).count();
                    println!(
                        "Translation: on (style {tl}), {done}/{} rows translated",
                        rows.len()
                    );
                }
            }
            println!("Styles:      {}", st.styles);
            println!("Dialogue:    {}", st.dialogue);
            println!("Comments:    {}", st.comments);
            println!("Duration:    {}", st.duration.to_hms_ms());
            println!("Characters:  {}", st.characters);
            println!(
                "Max CPS:     {:.1} ({} lines over {cps})",
                st.max_cps, st.lines_over_cps
            );
            println!("Overlaps:    {}", st.overlaps);
            if !st.missing_styles.is_empty() {
                println!("Missing styles: {}", st.missing_styles.join(", "));
            }
            if !l.warnings.is_empty() {
                println!("Parse warnings: {}", l.warnings.len());
            }
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Check {
            files,
            load: la,
            cps,
            strict,
        } => {
            let mut bad = false;
            for file in files {
                let l = load(&file, &la)?;
                let d = &l.doc;
                let mut errors = Vec::new();
                let mut warnings = Vec::new();
                for w in &l.warnings {
                    warnings.push(format!("line {}: {}", w.line, w.message));
                }
                for (i, e) in d.events().iter().enumerate() {
                    let ev = &e.event;
                    if ev.end < ev.start {
                        errors.push(format!(
                            "#{}: end before start ({} > {})",
                            i + 1,
                            ev.start,
                            ev.end
                        ));
                    }
                    if l.format.is_ass() && d.style(&ev.style).is_none() {
                        warnings.push(format!("#{}: unknown style '{}'", i + 1, ev.style));
                    }
                    let c = tags::cps(&ev.text, ev.duration().ms());
                    if !ev.is_comment() && c > cps {
                        let m = format!("#{}: {c:.1} CPS", i + 1);
                        if strict {
                            errors.push(m)
                        } else {
                            warnings.push(m)
                        }
                    }
                }
                for (a, b) in ops::overlapping_pairs(d) {
                    let ea = d.event(a).unwrap();
                    let eb = d.event(b).unwrap();
                    // typesetting often overlaps on purpose: only same-style lines
                    if ea.style == eb.style && ea.layer == eb.layer {
                        let m = format!("#{} and #{} overlap", a + 1, b + 1);
                        if strict {
                            errors.push(m)
                        } else {
                            warnings.push(m)
                        }
                    }
                }
                println!(
                    "{}: {} error(s), {} warning(s)",
                    file.display(),
                    errors.len(),
                    warnings.len()
                );
                for e in &errors {
                    println!("  error: {e}");
                }
                for w in &warnings {
                    println!("  warning: {w}");
                }
                bad |= !errors.is_empty();
            }
            Ok(if bad {
                ExitCode::FAILURE
            } else {
                ExitCode::SUCCESS
            })
        }
        Cmd::Roundtrip { files } => {
            let mut failed = 0;
            for f in &files {
                let bytes = std::fs::read(f).with_context(|| format!("reading {}", f.display()))?;
                let l = fs_formats::load_bytes(
                    &bytes,
                    f.extension().and_then(|e| e.to_str()),
                    &LoadOptions::default(),
                )?;
                if !l.format.is_ass() {
                    println!(
                        "SKIP {} ({} is not lossless by design)",
                        f.display(),
                        l.format.name()
                    );
                    continue;
                }
                let out = fs_formats::save_bytes(
                    &l.doc,
                    l.format,
                    &SaveOptions {
                        encoding: l.encoding,
                        bom: l.bom,
                        ..Default::default()
                    },
                );
                if out == bytes {
                    println!("OK   {} ({} lines)", f.display(), l.doc.event_count());
                } else {
                    let pos = out
                        .iter()
                        .zip(&bytes)
                        .position(|(a, b)| a != b)
                        .unwrap_or(out.len().min(bytes.len()));
                    println!("FAIL {} (first difference at byte {pos})", f.display());
                    failed += 1;
                }
            }
            Ok(if failed > 0 {
                ExitCode::FAILURE
            } else {
                ExitCode::SUCCESS
            })
        }
        Cmd::Lines {
            file,
            load: la,
            filter,
            strip,
        } => {
            let l = load(&file, &la)?;
            let d = &l.doc;
            let rows = build_filter(&filter)?.apply(d);
            println!(
                "{:>5} {:>2} {:<10} {:<10} {:>5} {:<14} {:<10} TEXT",
                "#", "L", "START", "END", "CPS", "STYLE", "ACTOR"
            );
            for i in rows {
                let e = d.event(i).unwrap();
                let text = if strip {
                    tags::display_text(&e.text)
                } else {
                    e.text.clone()
                };
                println!(
                    "{:>5}{}{:>2} {:<10} {:<10} {:>5.1} {:<14} {:<10} {}",
                    i + 1,
                    if e.is_comment() { "c" } else { " " },
                    e.layer,
                    e.start.to_ass(),
                    e.end.to_ass(),
                    tags::cps(&e.text, e.duration().ms()),
                    trunc(&e.style, 14),
                    trunc(&e.actor, 10),
                    text
                );
            }
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Fonts {
            file,
            load: la,
            dirs,
            collect,
        } => {
            let l = load(&file, &la)?;
            let mut db = fontdb::Database::new();
            db.load_system_fonts();
            for d in &dirs {
                db.load_fonts_dir(d);
            }
            let mut bad = false;
            if let Some(c) = &collect {
                std::fs::create_dir_all(c)?;
            }
            for (f, found) in fs_core::fonts::locate(&db, &fs_core::fonts::used_fonts(&l.doc)) {
                let face = format!(
                    "{} ({}{})",
                    f.family,
                    f.weight,
                    if f.italic { ", italic" } else { "" }
                );
                let Some(loc) = found else {
                    bad = true;
                    println!("MISSING  {face}");
                    continue;
                };
                print!(
                    "{}  {face}: {}",
                    if loc.synthetic { "FAUX   " } else { "OK     " },
                    loc.path.display()
                );
                if !loc.missing_glyphs.is_empty() {
                    bad = true;
                    print!(
                        "  missing glyphs: {}",
                        loc.missing_glyphs.iter().collect::<String>()
                    );
                }
                println!();
                if let (Some(c), Some(name)) = (&collect, loc.path.file_name()) {
                    std::fs::copy(&loc.path, c.join(name))
                        .with_context(|| format!("copying {}", loc.path.display()))?;
                }
            }
            Ok(if bad {
                ExitCode::from(1)
            } else {
                ExitCode::SUCCESS
            })
        }
        Cmd::Styles { file, load: la } => {
            let l = load(&file, &la)?;
            println!(
                "{:<24} {:<24} {:>6} {:>4} {:>5} {:>5} {:>3}  COLOURS (primary/secondary/outline/back)",
                "NAME", "FONT", "SIZE", "B/I", "OUTL", "SHAD", "AN"
            );
            for s in l.doc.styles() {
                println!(
                    "{:<24} {:<24} {:>6} {}{}  {:>5} {:>5} {:>3}  {} {} {} {}",
                    trunc(&s.name, 24),
                    trunc(&s.font, 24),
                    fs_core::model::fmt_f64(s.size),
                    if s.is_bold() { "B" } else { "-" },
                    if s.italic { "I" } else { "-" },
                    fs_core::model::fmt_f64(s.outline),
                    fs_core::model::fmt_f64(s.shadow),
                    s.alignment,
                    s.primary,
                    s.secondary,
                    s.outline_color,
                    s.back
                );
            }
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Convert {
            input,
            output,
            to,
            load: la,
            save,
        } => {
            let l = load(&input, &la)?;
            let fmt = match to {
                Some(f) => f.into(),
                None => {
                    let ext = output
                        .extension()
                        .and_then(|e| e.to_str())
                        .unwrap_or("")
                        .to_ascii_lowercase();
                    match ext.as_str() {
                        "ass" => Format::Ass,
                        "ssa" => Format::Ssa,
                        "srt" => Format::Srt,
                        "sub" => Format::MicroDvd,
                        _ => bail!(
                            "cannot infer output format from '{}'; pass --to",
                            output.display()
                        ),
                    }
                }
            };
            let mut opts = SaveOptions {
                bom: !save.no_bom,
                ..Default::default()
            };
            if let Some(e) = &save.out_encoding {
                opts.encoding = fs_formats::encoding::by_name(e)
                    .with_context(|| format!("unknown encoding '{e}'"))?;
            }
            if let Some(f) = &save.output_fps {
                opts.fps = parse_fps(f)?;
            } else if let Some(f) = l.declared_fps {
                opts.fps = Framerate::from_fps(f)?;
            }
            fs_formats::save_file(&l.doc, &output, fmt, &opts)?;
            eprintln!(
                "{} → {} ({}, {} lines)",
                input.display(),
                output.display(),
                fmt.name(),
                l.doc.event_count()
            );
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Shift {
            file,
            by,
            frames,
            timing,
            start_only,
            end_only,
            from_line,
            after,
            styles,
            out,
        } => {
            let l = load_default(&file)?;
            let fr = timing.framerate()?;
            let amount = match (&by, frames) {
                (Some(b), None) => ShiftAmount::Time(parse_time(b)?),
                (None, Some(n)) => ShiftAmount::Frames(
                    n,
                    fr.as_ref()
                        .context("--frames requires --fps or --timecodes")?,
                ),
                _ => bail!("pass either --by or --frames"),
            };
            let which = if start_only {
                ShiftWhich::Start
            } else if end_only {
                ShiftWhich::End
            } else {
                ShiftWhich::Both
            };
            let rows = match (from_line, after) {
                (Some(n), _) => Rows::From(n.saturating_sub(1)),
                (None, Some(t)) => Rows::StartingAt(parse_time(&t)?),
                _ => Rows::All,
            };
            let mut ed = Editor::new(l.doc.clone());
            let st = (!styles.is_empty()).then_some(styles.as_slice());
            let n = ed.edit("Shift times", |d| {
                ops::shift_times(d, &rows, amount, which, st)
            });
            eprintln!("shifted {n} line(s)");
            write_out(&file, &l, &ed.doc, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Sort { file, by, out } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            let key = match by {
                SortArg::Start => SortKey::Start,
                SortArg::End => SortKey::End,
                SortArg::Style => SortKey::Style,
                SortArg::Actor => SortKey::Actor,
                SortArg::Effect => SortKey::Effect,
                SortArg::Layer => SortKey::Layer,
                SortArg::Text => SortKey::Text,
            };
            ops::sort_events(&mut d, key, None);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Filter { file, filter, out } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            let keep = build_filter(&filter)?.apply(&d);
            let all: Vec<usize> = (0..d.event_count()).filter(|i| !keep.contains(i)).collect();
            let n = ops::delete_rows(&mut d, &all);
            eprintln!("kept {} line(s), removed {n}", keep.len());
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Replace {
            file,
            find,
            replace,
            regex,
            ignore_case,
            skip_tags,
            field,
            out,
        } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            let field = match field {
                FieldArg::Text => Field::Text,
                FieldArg::Style => Field::Style,
                FieldArg::Actor => Field::Actor,
                FieldArg::Effect => Field::Effect,
            };
            let mut m = TextMatcher::new(&find, regex, !ignore_case, field)?;
            m.strip_tags = skip_tags;
            let n = ops::replace(
                &mut d,
                &Rows::All,
                &ops::Replace {
                    matcher: m,
                    replacement: replace,
                    first_only: false,
                },
            );
            eprintln!("{n} replacement(s)");
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Resample {
            file,
            to,
            from,
            stretch,
            out,
        } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            let from = match from {
                Some(f) => parse_res(&f)?,
                None => d.play_res(),
            };
            let to = parse_res(&to)?;
            ops::resample(
                &mut d,
                &ops::Resample {
                    from,
                    to,
                    stretch,
                    margin: (0, 0, 0, 0),
                },
            );
            eprintln!("resampled {}x{} → {}x{}", from.0, from.1, to.0, to.1);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::ChangeFps {
            file,
            from,
            to,
            out,
        } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            ops::change_fps(&mut d, &parse_fps(&from)?, &parse_fps(&to)?);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Postprocess {
            file,
            lead_in,
            lead_out,
            adjacent,
            bias,
            keyframes,
            snap,
            timing,
            out,
        } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            let kfs = match &keyframes {
                Some(p) => Some(fs_core::keyframes::parse(&std::fs::read_to_string(p)?)?),
                None => None,
            };
            let fr = if kfs.is_some() {
                Some(timing.require()?)
            } else {
                None
            };
            let snap = parse_time(&snap)?;
            let pp = ops::PostProcess {
                lead_in: lead_in.as_deref().map(parse_time).transpose()?,
                lead_out: lead_out.as_deref().map(parse_time).transpose()?,
                adjacent_threshold: adjacent.as_deref().map(parse_time).transpose()?,
                adjacent_bias: bias,
                keyframes: kfs.as_deref().zip(fr.as_ref()),
                kf_before_start: snap,
                kf_after_start: snap,
                kf_before_end: snap,
                kf_after_end: snap,
                styles: Vec::new(),
            };
            ops::post_process(&mut d, &Rows::All, &pp);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Tl { cmd } => run_tl(cmd),
        #[cfg(feature = "media")]
        Cmd::Media { cmd } => media::run(cmd),
        #[cfg(feature = "lua")]
        Cmd::Lua(args) => lua::run(args),
        Cmd::Frame { value, timing } => {
            let fr = timing.require()?;
            if let Some(f) = value.strip_prefix(['f', 'F']) {
                let f: i64 = f.parse()?;
                println!("frame {f}");
                println!(
                    "  exact: {}",
                    Time(fr.time_at_frame(f, TimeType::Exact)).to_hms_ms()
                );
                println!(
                    "  start: {}  (line start that shows from this frame)",
                    Time(fr.time_at_frame(f, TimeType::Start)).to_ass()
                );
                println!(
                    "  end:   {}  (line end that shows up to this frame)",
                    Time(fr.time_at_frame(f, TimeType::End)).to_ass()
                );
                let (h, m, s, ff) = fr.smpte_at_frame(f);
                println!("  smpte: {h:02}:{m:02}:{s:02}:{ff:02}");
            } else {
                let t = parse_time(&value)?;
                println!("time {}", t.to_hms_ms());
                println!(
                    "  frame (exact): {}",
                    fr.frame_at_time(t.ms(), TimeType::Exact)
                );
                println!(
                    "  as line start: {}",
                    fr.frame_at_time(t.ms(), TimeType::Start)
                );
                println!(
                    "  as line end:   {}",
                    fr.frame_at_time(t.ms(), TimeType::End)
                );
            }
            Ok(ExitCode::SUCCESS)
        }
        Cmd::Timecodes {
            input,
            frames,
            output,
        } => {
            let fr = Framerate::parse_timecodes(&std::fs::read_to_string(&input)?)?;
            let s = fr.to_v2_string(frames);
            match output {
                Some(o) => std::fs::write(o, s)?,
                None => print!("{s}"),
            }
            Ok(ExitCode::SUCCESS)
        }
    }
}

fn run_tl(cmd: TlCmd) -> Result<ExitCode> {
    match cmd {
        TlCmd::Status { file } => {
            let l = load_default(&file)?;
            let d = &l.doc;
            match tlmode::tl_style(d) {
                None => println!("translation mode: off"),
                Some(st) => {
                    let rows = tlmode::rows(d);
                    let translated = rows.iter().filter(|r| r.translation.is_some()).count();
                    let doubtful = rows
                        .iter()
                        .filter(|r| {
                            r.translation
                                .is_some_and(|t| tlmode::is_doubtful(&d.event(t).unwrap().effect))
                        })
                        .count();
                    println!("translation mode: on (style '{st}')");
                    println!(
                        "rows: {}, translated: {translated}, doubtful: {doubtful}",
                        rows.len()
                    );
                }
            }
            Ok(ExitCode::SUCCESS)
        }
        TlCmd::Enable { file, out } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            tlmode::enable(&mut d);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        TlCmd::Disable { file, out } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            tlmode::disable(&mut d);
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        TlCmd::Paste {
            file,
            text,
            start,
            out,
        } => {
            let l = load_default(&file)?;
            let mut d = l.doc.clone();
            tlmode::enable(&mut d);
            let (t, _) = fs_formats::encoding::decode(&std::fs::read(&text)?, None);
            let lines: Vec<&str> = t.lines().map(|x| x.trim_end_matches('\r')).collect();
            let n = tlmode::paste_translation(&mut d, start.saturating_sub(1), &lines);
            eprintln!("filled {n} row(s)");
            write_out(&file, &l, &d, &out)?;
            Ok(ExitCode::SUCCESS)
        }
        TlCmd::ExtractOriginal { file } => {
            let l = load_default(&file)?;
            for r in tlmode::rows(&l.doc) {
                println!("{}", tlmode::original_text(&l.doc, r));
            }
            Ok(ExitCode::SUCCESS)
        }
    }
}

fn trunc(s: &str, n: usize) -> String {
    if s.chars().count() <= n {
        s.to_string()
    } else {
        let mut t: String = s.chars().take(n - 1).collect();
        t.push('…');
        t
    }
}
