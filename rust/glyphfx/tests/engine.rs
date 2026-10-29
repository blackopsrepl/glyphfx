use glyphfx::{Clip, EFFECTS};

fn frames(effect: &str) -> Vec<String> {
    let mut clip = Clip::new(effect, "GLYPH FX\nRust API", 16, 6).unwrap();
    let mut result = Vec::new();
    while let Some(frame) = clip.next_frame() {
        assert!(result.len() < 20_000, "{effect} did not complete");
        result.push(frame);
    }
    assert!(clip.is_finished());
    assert!(clip.next_frame().is_none());
    assert!(!result.is_empty(), "{effect} produced no frames");
    result
}

#[test]
fn all37_complete_deterministically_and_interleave() {
    assert_eq!(EFFECTS.len(), 37);
    for &effect in EFFECTS {
        let expected = frames(effect);
        let mut a = Clip::new(effect, "GLYPH FX\nRust API", 16, 6).unwrap();
        let mut b = Clip::new("matrix", "OTHER CLIP", 20, 8).unwrap();
        for frame in &expected {
            assert_eq!(a.next_frame().as_ref(), Some(frame), "{effect}");
            b.next_frame();
        }
        assert_eq!(a.next_frame(), None, "{effect}");
        eprintln!(
            "{effect}: {} real frames, deterministic/interleaved",
            expected.len()
        );
    }
}

#[test]
fn invalid_inputs_are_errors_and_live_clip_survives() {
    let mut good = Clip::new("wipe", "live", 12, 4).unwrap();
    for (effect, text, w, h) in [
        ("missing", "x", 10, 4),
        ("wipe", "x\0y", 10, 4),
        ("wipe", "", 10, 4),
        ("wipe", " ", 10, 4),
        ("wipe", "x", 0, 4),
        ("wipe", "x", 10, 0),
        ("wipe", "x", u16::MAX, u16::MAX),
        ("wipe", "\x1b]bad", 10, 4),
        ("wipe", "\x1b[38;2mX", 10, 4),
        ("wipe", "\x1b[0m", 10, 4),
    ] {
        assert!(Clip::new(effect, text, w, h).is_err(), "{effect} {text:?}");
    }
    assert!(good.next_frame().is_some());
}

#[test]
fn independent_threads_and_early_drop() {
    let handles: Vec<_> = ["wipe", "matrix", "beams"]
        .into_iter()
        .map(|effect| {
            std::thread::spawn(move || {
                let expected = frames(effect);
                let abandoned = Clip::new("print", "temporary", 16, 6).unwrap();
                drop(abandoned);
                assert_eq!(frames(effect), expected);
            })
        })
        .collect();
    for handle in handles {
        handle.join().unwrap();
    }
}
