//! Conversion between subtitle formats (Kainote's), Aegisub's export steps and resampling.

use std::path::Path;

use encoding_rs::Encoding;
use fs_core::Document;
use fs_core::ops;
use fs_core::vfr::Framerate;
use fs_formats::{Format, SaveOptions};

use super::styles::CATALOG_HEAD;
use super::{CONVERT, Session};
use crate::ffi::{ConvertSpec, ExportSpec, ResampleSpec};

impl Session {
    /// Switch the format the file is saved in (Kainote "Conversion"): the lines become what it
    /// holds, as one undo step, and the next Save asks for a name.
    pub fn convert(&mut self, format: &str) -> Result<(), String> {
        self.convert_with(format, &ConvertSpec::default())
    }

    /// Kainote's frame rate for files in frames with no rate of their own or a video's (0: 23.976).
    pub fn set_default_fps(&mut self, fps: f64) {
        self.default_fps = (fps > 0.0).then(|| Framerate::from_fps(fps).ok()).flatten();
    }

    /// A conversion with Kainote's settings, all in its one undo step.
    pub fn convert_with(&mut self, format: &str, spec: &ConvertSpec) -> Result<(), String> {
        let f = Format::from_name(format).ok_or_else(|| format!("unknown format {format}"))?;
        if self.tl {
            return Err("translation mode is on: turn it off to convert".into());
        }
        let opts = self.save_options();
        let before = (self.format, self.path.clone());
        let g = self.ed.generation();
        let to_ass = f.is_ass() && !self.format.is_ass();
        let ends = (spec.new_ends && f != Format::Tmp) || self.format == Format::Tmp;
        let style = (to_ass && !spec.style.is_empty())
            .then(|| {
                Document::parse(&format!("{CATALOG_HEAD}{}\n", spec.style))
                    .styles()
                    .next()
                    .cloned()
            })
            .flatten();
        if !f.is_ass() || to_ass || ends {
            self.edit(CONVERT, |d| {
                fs_formats::reduce(d, f, &opts);
                if to_ass {
                    if spec.res_x > 0 && spec.res_y > 0 {
                        d.set_info("PlayResX", &spec.res_x.to_string());
                        d.set_info("PlayResY", &spec.res_y.to_string());
                    }
                    if let Some(mut st) = style {
                        st.name = "Default".into();
                        match d.style_index("Default").and_then(|i| d.style_mut(i)) {
                            Some(old) => *old = st,
                            None => {
                                d.add_style(st);
                            }
                        }
                    }
                    if !spec.tags.is_empty() {
                        for i in 0..d.event_count() {
                            if let Some(e) = d.event_mut(i) {
                                e.text.insert_str(0, &spec.tags);
                            }
                        }
                    }
                }
                if ends {
                    fs_core::ops::new_end_times(d, i64::from(spec.ms_per_char));
                }
            });
        }
        // a conversion that changed no line (to ASS) has no step to undo
        if self.ed.generation() != g {
            self.convert_undo.push(before);
        }
        self.format = f;
        if let Some(p) = &mut self.path {
            p.set_extension(f.extension());
        }
        self.generation += 1;
        Ok(())
    }

    /// Whether the format was converted since the file was read or saved.
    pub fn converted(&self) -> bool {
        self.format != self.disk_format
    }

    /// Aegisub's Export: a copy through the Fix Styles / Transform Framerate steps, in the
    /// format `path`'s name says and `spec.encoding`. The open file, its path and saved state
    /// stay as they are.
    pub fn export_copy(&self, path: &str, spec: &ExportSpec, video: &[i64]) -> Result<(), String> {
        self.export_doc(self.ed.doc.clone(), path, spec, video)
    }

    /// `export_copy` of an already transformed document (automation export filters).
    pub(crate) fn export_doc(
        &self,
        mut doc: Document,
        path: &str,
        spec: &ExportSpec,
        video: &[i64],
    ) -> Result<(), String> {
        let rate = |s: &str| export_rate(s, video);
        if spec.fix_styles {
            ops::fix_styles(&mut doc);
        }
        if !spec.fps_to.is_empty() {
            ops::change_fps(&mut doc, &rate(&spec.fps_from)?, &rate(&spec.fps_to)?);
        }
        let p = Path::new(path);
        let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("");
        let opts = SaveOptions {
            encoding: Encoding::for_label(spec.encoding.as_bytes()).unwrap_or(self.encoding),
            bom: self.bom,
            fps: self
                .framerate
                .clone()
                .unwrap_or_else(|| Framerate::from_rational(24000, 1001, false).unwrap()),
            ..Default::default()
        };
        let name = (p.file_name().and_then(|n| n.to_str()))
            .unwrap_or("")
            .to_ascii_lowercase();
        let write = |bytes: Vec<u8>| std::fs::write(p, bytes).map_err(|e| format!("{path}: {e}"));
        // the rate of Encore's and TranStation's SMPTE times, drop-frame as asked
        let smpte = || {
            if spec.smpte_fps == "video" {
                return rate("video");
            }
            let fps = &spec.smpte_fps;
            match fps.split_once('/') {
                Some((n, d)) => n.trim().parse().ok().zip(d.trim().parse().ok()),
                None => fps.trim().parse().ok().map(|n| (n, 1)),
            }
            .ok_or_else(|| format!("{fps}: invalid frame rate"))
            .and_then(|(n, d)| {
                Framerate::from_rational(n, d, spec.smpte_drop).map_err(|e| e.to_string())
            })
        };
        // Aegisub's write-only formats, picked by the end of the name as there
        if name.ends_with(".encore.txt") {
            let text = fs_formats::export::write_encore(&doc, &smpte()?);
            return write(fs_formats::encoding::encode(
                &text,
                encoding_rs::UTF_8,
                true,
            ));
        }
        if name.ends_with(".transtation.txt") {
            let text = fs_formats::export::write_transtation(&doc, &smpte()?);
            return write(fs_formats::encoding::encode(&text, opts.encoding, opts.bom));
        }
        if ext.eq_ignore_ascii_case("stl") {
            let offset: Vec<u8> = (spec.ebu_timecode_offset.split(':'))
                .map(|v| v.trim().parse().unwrap_or(0))
                .collect();
            let settings = fs_formats::export::EbuSettings {
                tv_standard: spec.ebu_tv_standard,
                text_encoding: spec.ebu_text_encoding,
                max_line_length: spec.ebu_max_line_length.max(1) as usize,
                line_wrapping: spec.ebu_line_wrapping,
                translate_alignments: spec.ebu_translate_alignments,
                timecode_offset: std::array::from_fn(|k| offset.get(k).copied().unwrap_or(0)),
                inclusive_end_times: spec.ebu_inclusive_end_times,
                display_standard: spec.ebu_display_standard,
            };
            return write(fs_formats::export::write_ebu_stl(&doc, &settings)?);
        }
        if ext.eq_ignore_ascii_case("txt") {
            // Aegisub's plain text export; TMP and MPL2 come from converting the subtitles
            let text = fs_formats::write_plain_text(&doc);
            let bytes = fs_formats::encoding::encode(&text, opts.encoding, opts.bom);
            return std::fs::write(p, bytes).map_err(|e| format!("{path}: {e}"));
        }
        let format = Format::from_name(ext).unwrap_or(Format::Ass);
        fs_formats::save_file(&doc, p, format, &opts)
            .map(drop)
            .map_err(|e| format!("{path}: {e}"))
    }

    pub fn resample(&mut self, spec: &ResampleSpec) {
        let matrices = !spec.from_matrix.is_empty() && !spec.to_matrix.is_empty();
        let r = ops::Resample {
            from: (spec.from_x, spec.from_y),
            to: (spec.to_x, spec.to_y),
            margin: [spec.margin_l, spec.margin_r, spec.margin_t, spec.margin_b],
            ar_mode: match spec.ar_mode {
                1 => ops::ArMode::AddBorder,
                2 => ops::ArMode::RemoveBorder,
                3 => ops::ArMode::Manual,
                _ => ops::ArMode::Stretch,
            },
            matrix: matrices.then(|| (spec.from_matrix.clone(), spec.to_matrix.clone())),
        };
        self.edit("Resample resolution", |d| ops::resample(d, &r));
    }
}

/// Transform Framerate's rates: "video" is the video's timecodes, else a rate as typed.
pub(crate) fn export_rate(s: &str, video: &[i64]) -> Result<Framerate, String> {
    if s == "video" {
        Framerate::from_timecodes(video.to_vec())
    } else {
        Framerate::parse(s)
    }
    .map_err(|e| format!("{s}: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn a_conversion_to_ass_takes_kainotes_settings() {
        let p = std::env::temp_dir().join(format!("fsb-conv-{}.srt", std::process::id()));
        std::fs::write(
            &p,
            "1\n00:00:01,000 --> 00:00:09,000\nabcd\n\n2\n00:00:03,000 --> 00:00:04,000\nxy\n",
        )
        .unwrap();
        let mut s = Session::default();
        s.open(p.to_str().unwrap()).unwrap();
        let spec = ConvertSpec {
            res_x: 1280,
            res_y: 720,
            style: "Style: Mine,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1".into(),
            tags: r"{e1}".into(),
            new_ends: true,
            ms_per_char: 1000,
        };
        s.convert_with("ass", &spec).unwrap();
        assert_eq!(s.file_info().play_res_x, 1280);
        assert_eq!(s.row(0).text, r"{e1}abcd");
        assert_eq!((s.row(0).end, s.row(1).end), (3000, 5000)); // 4 s cut at the next start; 2 s
        let st = s.styles();
        assert_eq!((st[0].name.as_str(), st[0].size), ("Default", 40.0));
        s.undo(); // one step back to the SRT as read
        assert_eq!((s.row(0).end, s.row(0).text.as_str()), (9000, "abcd"));
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn a_conversion_saves_under_a_new_name() {
        let mut s = session(ASS);
        assert!(!s.converted() && !s.file_info().modified);
        let n = s.row_count();
        s.convert("srt").unwrap();
        // the lines are what SRT holds, the next Save asks for a name
        assert!(s.converted() && s.file_info().modified);
        assert!(s.lossless("a.srt") && s.row_count() <= n);
        s.undo();
        assert_eq!(s.row_count(), n);
        // the format goes back with the lines, and comes again with redo
        assert!(!s.converted());
        s.redo();
        assert!(s.converted());
        s.undo_to(1);
        assert!(!s.converted());
        let p = std::env::temp_dir().join(format!("fsb-conv-{}.srt", std::process::id()));
        s.convert("srt").unwrap();
        s.save(p.to_str().unwrap()).unwrap();
        assert!(!s.converted() && !s.file_info().modified);
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn export_copy_filters_without_touching_the_file() {
        let s = session(ASS);
        let path = s.file_info().path;
        let out = std::env::temp_dir().join(format!("fsb-export-{}.srt", std::process::id()));
        let spec = ExportSpec {
            fix_styles: true,
            fps_from: "23.976".into(),
            fps_to: "25".into(),
            encoding: "UTF-8".into(),
            ..Default::default()
        };
        s.export_copy(out.to_str().unwrap(), &spec, &[]).unwrap();
        let srt = std::fs::read_to_string(&out).unwrap();
        // 1 s at 23.976 fps is 0.959 s at 25 fps
        assert!(srt.contains("00:00:00,95"), "{srt}");
        assert_eq!(s.file_info().path, path);
        assert!(!s.file_info().modified);
        std::fs::remove_file(out).unwrap();
    }
}
