//! `fsed media …`: video/audio commands (feature `media`).

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{Context, Result, bail};
use clap::Subcommand;
use fs_core::Time;
use fs_core::vfr::TimeType;
use fs_media::{Audio, SubtitleRenderer, Video};

#[derive(Subcommand)]
pub enum MediaCmd {
    /// List streams and show video/audio properties.
    Info { file: PathBuf },
    /// Save one frame (optionally with subtitles rendered by libass) as PNG.
    Frame {
        video: PathBuf,
        /// Frame number.
        #[arg(long, conflicts_with = "time")]
        frame: Option<i64>,
        /// Time (the frame shown at that time).
        #[arg(long)]
        time: Option<String>,
        /// Subtitles to render on top.
        #[arg(long)]
        subs: Option<PathBuf>,
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Write the keyframe list (Aegisub keyframes format).
    Keyframes {
        video: PathBuf,
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Write v2 timecodes of the video.
    Timecodes {
        video: PathBuf,
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
    /// Save part of the audio as 16-bit WAV (Aegisub "Save audio clip").
    AudioClip {
        file: PathBuf,
        #[arg(long)]
        start: String,
        #[arg(long)]
        end: String,
        /// Downmix to mono.
        #[arg(long)]
        mono: bool,
        #[arg(short, long)]
        output: PathBuf,
    },
}

fn progress(label: &'static str) -> impl FnMut(i64, i64) -> bool {
    let mut last = -1;
    move |done, total| {
        let pct = if total > 0 {
            (done as f64 / total as f64 * 100.0).clamp(0.0, 100.0) as i64
        } else {
            0
        };
        if pct != last && pct % 10 == 0 {
            eprint!("\r{label}: {pct}%   ");
            last = pct;
        }
        true
    }
}

fn open_video(p: &Path) -> Result<Video> {
    let cache = fs_media::default_cache_dir();
    let v = Video::open(p, -1, Some(&cache), progress("indexing video"))
        .with_context(|| format!("opening {}", p.display()))?;
    eprint!("\r                          \r");
    Ok(v)
}

pub fn run(cmd: MediaCmd) -> Result<ExitCode> {
    match cmd {
        MediaCmd::Info { file } => {
            println!("File: {}", file.display());
            for t in fs_media::list_tracks(&file)? {
                let mut extra = String::new();
                if !t.language.is_empty() {
                    extra += &format!(" [{}]", t.language);
                }
                if !t.title.is_empty() {
                    extra += &format!(" \"{}\"", t.title);
                }
                if t.is_default {
                    extra += " (default)";
                }
                println!("  #{} {} {}{extra}", t.index, t.kind, t.codec);
            }
            let tracks = fs_media::list_tracks(&file)?;
            if tracks.iter().any(|t| t.kind == "video") {
                let v = open_video(&file)?;
                let p = v.props();
                println!(
                    "Video:      {}x{} {} (SAR {}:{}), matrix {}",
                    p.width,
                    p.height,
                    p.pixel_format,
                    p.sar_num,
                    p.sar_den,
                    if p.matrix.is_empty() {
                        "unspecified"
                    } else {
                        p.matrix.as_str()
                    }
                );
                println!(
                    "Frames:     {} at {}/{} ({:.3} fps){}",
                    p.num_frames,
                    p.fps_num,
                    p.fps_den,
                    p.fps_num as f64 / p.fps_den.max(1) as f64,
                    if v.is_vfr() {
                        ", variable frame rate"
                    } else {
                        ""
                    }
                );
                println!(
                    "Duration:   {}",
                    Time((p.duration_s * 1000.0).round() as i64).to_hms_ms()
                );
                println!("Keyframes:  {}", v.keyframes().len());
                if v.linear_decoding() {
                    println!("Warning:    seeking is unreliable, decoding linearly (slow)");
                }
            }
            if tracks.iter().any(|t| t.kind == "audio") {
                let a = Audio::open(
                    &file,
                    -1,
                    Some(&fs_media::default_cache_dir()),
                    progress("indexing audio"),
                )?;
                eprint!("\r                          \r");
                let p = a.props();
                println!(
                    "Audio:      {} Hz, {} ch, {} {}-bit, {} samples ({})",
                    p.sample_rate,
                    p.channels,
                    if p.is_float { "float" } else { "int" },
                    p.bits,
                    p.num_samples,
                    Time(p.num_samples * 1000 / p.sample_rate.max(1) as i64).to_hms_ms()
                );
            }
            let fonts = fs_media::attached_fonts(&file)?;
            if !fonts.is_empty() {
                println!(
                    "Fonts:      {}",
                    fonts
                        .iter()
                        .map(|(n, _)| n.as_str())
                        .collect::<Vec<_>>()
                        .join(", ")
                );
            }
        }
        MediaCmd::Frame {
            video,
            frame,
            time,
            subs,
            output,
        } => {
            let mut v = open_video(&video)?;
            let fr = v.framerate();
            let n = match (frame, time) {
                (Some(f), _) => f,
                (None, Some(t)) => fr.frame_at_time(crate::parse_time(&t)?.ms(), TimeType::Exact),
                (None, None) => bail!("give --frame or --time"),
            };
            if n < 0 || n >= v.num_frames() {
                bail!("frame {n} out of range (0..{})", v.num_frames());
            }
            let (w, h) = (v.width(), v.height());
            let mut rgba = v.frame(n)?;
            if let Some(s) = subs {
                let loaded = fs_formats::load_file(&s, &Default::default())?;
                let mut r = SubtitleRenderer::new()?;
                for (name, data) in fs_media::attached_fonts(&video).unwrap_or_default() {
                    r.add_font(&name, &data);
                }
                r.set_script(&loaded.doc.to_ass_string())?;
                // libass shows a line on a frame from the frame's start time
                let ms = fr.time_at_frame(n, TimeType::Exact);
                r.blend(ms, w, h, (w, h), &mut rgba)?;
            }
            std::fs::write(&output, png_rgba(w as u32, h as u32, &rgba))?;
            eprintln!(
                "frame {n} ({}) -> {}",
                Time(fr.time_at_frame(n, TimeType::Exact)).to_hms_ms(),
                output.display()
            );
        }
        MediaCmd::Keyframes { video, output } => {
            let v = open_video(&video)?;
            let fr = v.framerate();
            let mut s = format!("# keyframe format v1\nfps {:.6}\n", fr.fps());
            for k in v.keyframes() {
                s += &format!("{k}\n");
            }
            write_or_print(output, &s)?;
        }
        MediaCmd::Timecodes { video, output } => {
            let v = open_video(&video)?;
            let mut s = String::from("# timecode format v2\n");
            for t in v.timestamps_ms() {
                s += &format!("{t}\n");
            }
            write_or_print(output, &s)?;
        }
        MediaCmd::AudioClip {
            file,
            start,
            end,
            mono,
            output,
        } => {
            let a = Audio::open(
                &file,
                -1,
                Some(&fs_media::default_cache_dir()),
                progress("indexing audio"),
            )?;
            eprint!("\r                          \r");
            let p = *a.props();
            let rate = p.sample_rate as i64;
            let s = crate::parse_time(&start)?.ms() * rate / 1000;
            let e = (crate::parse_time(&end)?.ms() * rate / 1000).min(p.num_samples);
            if e <= s {
                bail!("empty range");
            }
            let ch = p.channels as usize;
            let mut buf = vec![0f32; (e - s) as usize * ch];
            a.read(s, &mut buf)?;
            let to_i16 = |x: f32| (x * 32767.0).round().clamp(-32768.0, 32767.0) as i16;
            let (channels, samples): (u16, Vec<i16>) = if mono {
                (
                    1,
                    buf.chunks_exact(ch)
                        .map(|f| to_i16(f.iter().sum::<f32>() / ch as f32))
                        .collect(),
                )
            } else {
                (ch as u16, buf.iter().map(|&x| to_i16(x)).collect())
            };
            let mut f = std::io::BufWriter::new(std::fs::File::create(&output)?);
            fs_audio::wav::write_i16(&mut f, p.sample_rate as u32, channels, &samples)?;
            eprintln!("{} samples -> {}", e - s, output.display());
        }
    }
    Ok(ExitCode::SUCCESS)
}

fn write_or_print(output: Option<PathBuf>, s: &str) -> Result<()> {
    match output {
        Some(o) => std::fs::write(o, s)?,
        None => print!("{s}"),
    }
    Ok(())
}

/// Minimal PNG encoder (RGBA8, zlib "stored" blocks — large but simple).
fn png_rgba(w: u32, h: u32, rgba: &[u8]) -> Vec<u8> {
    fn crc32(data: &[u8]) -> u32 {
        let mut c = 0xFFFF_FFFFu32;
        for &b in data {
            c ^= b as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 {
                    0xEDB8_8320 ^ (c >> 1)
                } else {
                    c >> 1
                };
            }
        }
        !c
    }
    fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        let start = out.len();
        out.extend_from_slice(kind);
        out.extend_from_slice(data);
        let crc = crc32(&out[start..]);
        out.extend_from_slice(&crc.to_be_bytes());
    }
    let mut raw = Vec::with_capacity((w as usize * 4 + 1) * h as usize);
    for row in rgba.chunks_exact(w as usize * 4).take(h as usize) {
        raw.push(0); // filter: none
        raw.extend_from_slice(row);
    }
    let mut z = vec![0x78, 0x01];
    let blocks: Vec<&[u8]> = raw.chunks(65535).collect();
    for (i, b) in blocks.iter().enumerate() {
        z.push(u8::from(i + 1 == blocks.len()));
        z.extend_from_slice(&(b.len() as u16).to_le_bytes());
        z.extend_from_slice(&(!(b.len() as u16)).to_le_bytes());
        z.extend_from_slice(b);
    }
    let (mut a, mut bsum) = (1u32, 0u32);
    for &x in &raw {
        a = (a + x as u32) % 65521;
        bsum = (bsum + a) % 65521;
    }
    z.extend_from_slice(&((bsum << 16) | a).to_be_bytes());
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &z);
    chunk(&mut out, b"IEND", &[]);
    out
}
