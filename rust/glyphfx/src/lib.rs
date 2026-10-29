//! Pull-based text effects from the C17 engine. No subprocess, TTY or sleeping.
//! Each frame is a complete ANSI-styled canvas, rows top-first, newline joined.
//! The clock advances by 1/60 second per frame; default seed is 1.
#![forbid(unsafe_code)]

/// All engine effects, in registry order.
pub const EFFECTS: &[&str] = &[
    "beams",
    "binarypath",
    "blackhole",
    "bouncyballs",
    "bubbles",
    "burn",
    "colorshift",
    "crumble",
    "decrypt",
    "errorcorrect",
    "expand",
    "fireworks",
    "highlight",
    "laseretch",
    "matrix",
    "middleout",
    "orbittingvolley",
    "overflow",
    "pour",
    "print",
    "rain",
    "randomsequence",
    "rings",
    "scattered",
    "slice",
    "slide",
    "smoke",
    "spotlights",
    "spray",
    "swarm",
    "sweep",
    "synthgrid",
    "thunderstorm",
    "unstable",
    "vhstape",
    "waves",
    "wipe",
];

/// An independent finite default-config effect. Creation builds only simulation
/// state; frames are generated on demand. Canvas is limited to 65536 cells.
/// Input follows the engine's codepoint-per-cell layout; consumers should decode
/// ANSI using their terminal's Unicode display widths. Empty/NUL input is rejected.
pub struct Clip(glyphfx_sys::Session);

impl Clip {
    pub fn new(effect: &str, text: &str, width: u16, height: u16) -> Result<Self, String> {
        Self::with_seed(effect, text, width, height, 1)
    }

    pub fn with_seed(
        effect: &str,
        text: &str,
        width: u16,
        height: u16,
        seed: u64,
    ) -> Result<Self, String> {
        if !EFFECTS.contains(&effect) {
            return Err(format!("unknown effect: {effect}"));
        }
        glyphfx_sys::Session::new(effect, text, width, height, seed).map(Self)
    }

    pub fn next_frame(&mut self) -> Option<String> {
        self.0.next_frame()
    }
    /// True after a pull observes engine completion. Further pulls return None.
    pub fn is_finished(&self) -> bool {
        self.0.is_finished()
    }
}
