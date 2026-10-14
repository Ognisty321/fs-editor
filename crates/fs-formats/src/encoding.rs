//! Text encoding detection and conversion.
//!
//! Detection order: byte-order mark → strict UTF-8 → statistical guess
//! (`chardetng`, with a Polish/Central-European bias since that's where most
//! legacy-encoded subtitles that reach this editor come from).

use encoding_rs::{Encoding, UTF_8, UTF_16BE, UTF_16LE};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Decoded {
    pub encoding: &'static Encoding,
    pub bom: bool,
}

/// Detect the encoding of `bytes` without decoding.
pub fn detect(bytes: &[u8]) -> Decoded {
    if let Some((enc, _)) = Encoding::for_bom(bytes) {
        return Decoded {
            encoding: enc,
            bom: true,
        };
    }
    // UTF-16 without BOM: lots of zero bytes in alternating positions.
    if bytes.len() >= 4 {
        let n = bytes.len().min(4096) & !1;
        let (mut even0, mut odd0) = (0usize, 0usize);
        for (i, b) in bytes[..n].iter().enumerate() {
            if *b == 0 {
                if i % 2 == 0 { even0 += 1 } else { odd0 += 1 }
            }
        }
        if odd0 > n / 8 && even0 * 8 <= odd0 / 8 {
            return Decoded {
                encoding: UTF_16LE,
                bom: false,
            };
        }
        if even0 > n / 8 && odd0 * 8 <= even0 / 8 {
            return Decoded {
                encoding: UTF_16BE,
                bom: false,
            };
        }
    }
    if std::str::from_utf8(bytes).is_ok() || valid_utf8_but_truncated(bytes) {
        return Decoded {
            encoding: UTF_8,
            bom: false,
        };
    }
    let mut det = chardetng::EncodingDetector::new(chardetng::Iso2022JpDetection::Deny);
    det.feed(bytes, true);
    Decoded {
        encoding: det.guess(Some(b"pl"), chardetng::Utf8Detection::Deny),
        bom: false,
    }
}

/// UTF-8 that is only invalid because the final multi-byte sequence was cut.
fn valid_utf8_but_truncated(bytes: &[u8]) -> bool {
    match std::str::from_utf8(bytes) {
        Ok(_) => true,
        Err(e) => e.error_len().is_none() && bytes.len() - e.valid_up_to() < 4,
    }
}

/// Decode bytes; returns text (BOM removed) and what was detected. When
/// `forced` is given that encoding is used regardless of detection.
pub fn decode(bytes: &[u8], forced: Option<&'static Encoding>) -> (String, Decoded) {
    let det = detect(bytes);
    let det = match forced {
        Some(f) => Decoded {
            encoding: f,
            bom: det.bom && det.encoding == f,
        },
        None => det,
    };
    let body = if det.bom {
        let (_, bom_len) = Encoding::for_bom(bytes).unwrap_or((det.encoding, 0));
        &bytes[bom_len..]
    } else {
        bytes
    };
    let (text, _) = det.encoding.decode_without_bom_handling(body);
    (text.into_owned(), det)
}

/// Encode text; writes a BOM for UTF encodings when `bom` is set.
pub fn encode(text: &str, encoding: &'static Encoding, bom: bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len() + 3);
    if encoding == UTF_16LE {
        if bom {
            out.extend_from_slice(&[0xFF, 0xFE]);
        }
        for u in text.encode_utf16() {
            out.extend_from_slice(&u.to_le_bytes());
        }
        return out;
    }
    if encoding == UTF_16BE {
        if bom {
            out.extend_from_slice(&[0xFE, 0xFF]);
        }
        for u in text.encode_utf16() {
            out.extend_from_slice(&u.to_be_bytes());
        }
        return out;
    }
    if encoding == UTF_8 && bom {
        out.extend_from_slice(b"\xEF\xBB\xBF");
    }
    let (bytes, _, _) = encoding.encode(text);
    out.extend_from_slice(&bytes);
    out
}

/// Look up an encoding by WHATWG label (`utf-8`, `windows-1250`, `cp1250`, …).
pub fn by_name(name: &str) -> Option<&'static Encoding> {
    let n = name.trim().to_ascii_lowercase();
    let n = match n.as_str() {
        "cp1250" => "windows-1250",
        "cp1251" => "windows-1251",
        "cp1252" => "windows-1252",
        "utf8" => "utf-8",
        "utf16" | "utf-16" => "utf-16le",
        other => other,
    };
    Encoding::for_label(n.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16_roundtrip() {
        let s = "Zażółć gęślą jaźń";
        for enc in [UTF_16LE, UTF_16BE, UTF_8] {
            for bom in [true, false] {
                let b = encode(s, enc, bom);
                let (t, d) = decode(&b, None);
                assert_eq!(t, s, "{} bom={bom}", enc.name());
                assert_eq!(d.encoding, enc);
                assert_eq!(d.bom, bom);
            }
        }
    }

    #[test]
    fn cp1250_polish() {
        let s = "Zażółć gęślą jaźń. Pchnąć w tę łódź jeża lub ośm skrzyń fig.\n";
        let enc = encoding_rs::WINDOWS_1250;
        let b = encode(s, enc, false);
        let (t, d) = decode(&b, None);
        assert_eq!(d.encoding, enc);
        assert_eq!(t, s);
    }
}
