//! Playback against the real default output device (skipped when there is none).

use std::time::{Duration, Instant};

use fs_audio::{PcmBuffer, Player};

#[test]
fn position_advances_and_playback_stops_at_end() {
    let rate = 44100;
    let pcm = PcmBuffer::new(rate, vec![0; rate as usize * 2]);
    let player = match Player::new(pcm, None) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("skipping: {e}");
            return;
        }
    };
    player.set_volume(0.0);
    player.play_ms(500, 900);
    assert!(player.is_playing());
    let t0 = Instant::now();
    let mut last = player.position_ms();
    assert!((500..=900).contains(&last), "{last}");
    let mut advanced = false;
    while player.is_playing() && t0.elapsed() < Duration::from_secs(3) {
        std::thread::sleep(Duration::from_millis(20));
        let p = player.position_ms();
        assert!(p >= last, "position went back: {p} < {last}");
        advanced |= p > 500;
        last = p;
    }
    assert!(!player.is_playing(), "did not stop at the end of the range");
    assert!(advanced);
    assert!(
        t0.elapsed() >= Duration::from_millis(300),
        "{:?}",
        t0.elapsed()
    );
    eprintln!(
        "device: {} @ {} Hz",
        player.device_name(),
        player.device_rate()
    );
}
