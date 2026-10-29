use glyphfx::{Clip, EFFECTS};
#[test]
fn small_canvases() {
    for &effect in EFFECTS {
        for (w, h) in [(1, 1), (1, 4), (4, 1), (2, 2)] {
            eprintln!("{effect} {w}x{h}");
            match Clip::new(effect, "X", w, h) {
                Ok(mut clip) => {
                    for _ in 0..20_000 {
                        if clip.next_frame().is_none() {
                            break;
                        }
                    }
                    assert!(clip.is_finished(), "{effect} {w}x{h}");
                }
                Err(error) => assert!(!error.is_empty()),
            }
        }
    }
}
