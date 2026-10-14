//! RIFF/WAVE reading and writing (PCM 8/16/24/32-bit, IEEE float 32/64,
//! WAVE_FORMAT_EXTENSIBLE). Used for tests, for opening plain WAV files
//! without FFmpeg, and for "Save audio clip".

use std::io::Write;

use crate::pcm::MemorySource;

#[derive(Debug, thiserror::Error)]
pub enum WavError {
    #[error("not a RIFF/WAVE file")]
    NotWave,
    #[error("missing {0} chunk")]
    MissingChunk(&'static str),
    #[error("unsupported WAV format (tag {tag}, {bits} bits)")]
    Unsupported { tag: u16, bits: u16 },
    #[error(transparent)]
    Io(#[from] std::io::Error),
}

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// Parse a WAV file held in memory.
pub fn read(bytes: &[u8]) -> Result<MemorySource, WavError> {
    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err(WavError::NotWave);
    }
    let mut fmt: Option<&[u8]> = None;
    let mut data: Option<&[u8]> = None;
    let mut p = 12;
    while p + 8 <= bytes.len() {
        let id = &bytes[p..p + 4];
        let size = u32le(bytes, p + 4) as usize;
        let body = &bytes[p + 8..(p + 8 + size).min(bytes.len())];
        match id {
            b"fmt " => fmt = Some(body),
            // a data size of 0/0xFFFFFFFF is written by streaming encoders
            b"data" => {
                data = Some(if size == 0 || size == u32::MAX as usize {
                    &bytes[p + 8..]
                } else {
                    body
                })
            }
            _ => {}
        }
        p += 8 + size + (size & 1);
    }
    let fmt = fmt
        .filter(|f| f.len() >= 16)
        .ok_or(WavError::MissingChunk("fmt"))?;
    let data = data.ok_or(WavError::MissingChunk("data"))?;
    let mut tag = u16le(fmt, 0);
    let channels = u16le(fmt, 2).max(1);
    let rate = u32le(fmt, 4);
    let bits = u16le(fmt, 14);
    if tag == 0xFFFE && fmt.len() >= 26 {
        tag = u16le(fmt, 24); // first two bytes of the sub-format GUID
    }
    let bps = (bits as usize).div_ceil(8);
    if bps == 0 {
        return Err(WavError::Unsupported { tag, bits });
    }
    let n = data.len() / bps / channels as usize * channels as usize;
    let mut out = Vec::with_capacity(n);
    let s = |i: usize| &data[i * bps..(i + 1) * bps];
    match (tag, bps) {
        (1, 1) => out.extend((0..n).map(|i| (s(i)[0] as f32 - 128.0) / 128.0)),
        (1, 2) => {
            out.extend((0..n).map(|i| i16::from_le_bytes([s(i)[0], s(i)[1]]) as f32 / 32768.0))
        }
        (1, 3) => out.extend((0..n).map(|i| {
            let b = s(i);
            (i32::from_le_bytes([0, b[0], b[1], b[2]]) >> 8) as f32 / 8_388_608.0
        })),
        (1, 4) => out.extend(
            (0..n).map(|i| i32::from_le_bytes(s(i).try_into().unwrap()) as f32 / 2_147_483_648.0),
        ),
        (3, 4) => out.extend((0..n).map(|i| f32::from_le_bytes(s(i).try_into().unwrap()))),
        (3, 8) => out.extend((0..n).map(|i| f64::from_le_bytes(s(i).try_into().unwrap()) as f32)),
        _ => return Err(WavError::Unsupported { tag, bits }),
    }
    Ok(MemorySource::new(rate, channels, out))
}

/// Write mono or interleaved 16-bit PCM.
pub fn write_i16(
    w: &mut impl Write,
    rate: u32,
    channels: u16,
    samples: &[i16],
) -> std::io::Result<()> {
    let data_len = (samples.len() * 2) as u32;
    w.write_all(b"RIFF")?;
    w.write_all(&(36 + data_len).to_le_bytes())?;
    w.write_all(b"WAVEfmt ")?;
    w.write_all(&16u32.to_le_bytes())?;
    w.write_all(&1u16.to_le_bytes())?;
    w.write_all(&channels.to_le_bytes())?;
    w.write_all(&rate.to_le_bytes())?;
    w.write_all(&(rate * channels as u32 * 2).to_le_bytes())?;
    w.write_all(&(channels * 2).to_le_bytes())?;
    w.write_all(&16u16.to_le_bytes())?;
    w.write_all(b"data")?;
    w.write_all(&data_len.to_le_bytes())?;
    let mut buf = Vec::with_capacity(samples.len() * 2);
    for s in samples {
        buf.extend_from_slice(&s.to_le_bytes());
    }
    w.write_all(&buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pcm::AudioSource;

    #[test]
    fn roundtrip_i16() {
        let mut v = Vec::new();
        write_i16(&mut v, 44100, 2, &[0, 32767, -32768, 100]).unwrap();
        let src = read(&v).unwrap();
        assert_eq!(
            (src.sample_rate(), src.channels(), src.num_frames()),
            (44100, 2, 2)
        );
        let mut out = [0f32; 4];
        src.fill(0, &mut out);
        assert_eq!(out[0], 0.0);
        assert!((out[1] - 32767.0 / 32768.0).abs() < 1e-6);
        assert_eq!(out[2], -1.0);
    }

    #[test]
    fn float_and_24bit() {
        // build a 24-bit extensible header by hand
        let mut v = Vec::new();
        let samples: [i32; 2] = [8_388_607, -8_388_608];
        let data: Vec<u8> = samples
            .iter()
            .flat_map(|s| s.to_le_bytes()[..3].to_vec())
            .collect();
        let mut fmt = Vec::new();
        fmt.extend_from_slice(&0xFFFEu16.to_le_bytes());
        fmt.extend_from_slice(&1u16.to_le_bytes());
        fmt.extend_from_slice(&48000u32.to_le_bytes());
        fmt.extend_from_slice(&(48000u32 * 3).to_le_bytes());
        fmt.extend_from_slice(&3u16.to_le_bytes());
        fmt.extend_from_slice(&24u16.to_le_bytes());
        fmt.extend_from_slice(&22u16.to_le_bytes());
        fmt.extend_from_slice(&24u16.to_le_bytes());
        fmt.extend_from_slice(&4u32.to_le_bytes());
        fmt.extend_from_slice(&[
            1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71,
        ]);
        v.extend_from_slice(b"RIFF");
        v.extend_from_slice(&((4 + 8 + fmt.len() + 8 + data.len()) as u32).to_le_bytes());
        v.extend_from_slice(b"WAVEfmt ");
        v.extend_from_slice(&(fmt.len() as u32).to_le_bytes());
        v.extend_from_slice(&fmt);
        v.extend_from_slice(b"data");
        v.extend_from_slice(&(data.len() as u32).to_le_bytes());
        v.extend_from_slice(&data);
        let src = read(&v).unwrap();
        let mut out = [0f32; 2];
        src.fill(0, &mut out);
        assert!((out[0] - 1.0).abs() < 1e-6 && out[1] == -1.0);
        assert!(matches!(read(b"RIFF\0\0\0\0WAVX"), Err(WavError::NotWave)));
    }
}
