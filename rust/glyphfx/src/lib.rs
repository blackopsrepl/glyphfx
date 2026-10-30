//! Pull-based text effects from the C17 engine. No subprocess, TTY or sleeping.
//! Each frame is a complete ANSI-styled canvas, rows top-first, newline joined.
//! The clock advances by 1/60 second per frame; default seed is 1.
#![forbid(unsafe_code)]

/// All engine effects, in registry order: the 37 ttfx effects, then the four
/// glyphfx originals (see `ORIGINALS`).
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
    "bannerwave",
    "burnout",
    "comet",
    "copper",
];

/// The glyphfx originals: effects with no ttfx counterpart, outside the
/// byte-parity contract. `burnout` is the one exit effect (the text burns away).
pub const ORIGINALS: &[&str] = &["bannerwave", "burnout", "comet", "copper"];

/// Where the input text block settles inside the canvas.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Anchor {
    North,
    NorthEast,
    East,
    SouthEast,
    South,
    SouthWest,
    West,
    #[default]
    NorthWest,
    Center,
}

/// An independent finite default-config effect. Creation builds only simulation
/// state; frames are generated on demand. Canvas is limited to 65536 cells.
/// Input follows the engine's codepoint-per-cell layout; consumers should decode
/// ANSI using their terminal's Unicode display widths. Empty/NUL input is rejected.
pub struct Clip(glyphfx_sys::Session);

impl Clip {
    /// Seed 1, text anchored top-left.
    pub fn new(effect: &str, text: &str, width: u16, height: u16) -> Result<Self, String> {
        Self::with_options(effect, text, width, height, 1, Anchor::NorthWest)
    }

    pub fn with_seed(
        effect: &str,
        text: &str,
        width: u16,
        height: u16,
        seed: u64,
    ) -> Result<Self, String> {
        Self::with_options(effect, text, width, height, seed, Anchor::NorthWest)
    }

    pub fn with_options(
        effect: &str,
        text: &str,
        width: u16,
        height: u16,
        seed: u64,
        anchor: Anchor,
    ) -> Result<Self, String> {
        if !EFFECTS.contains(&effect) {
            return Err(format!("unknown effect: {effect}"));
        }
        glyphfx_sys::Session::new(effect, text, width, height, seed, anchor as u8).map(Self)
    }

    pub fn next_frame(&mut self) -> Option<String> {
        self.0.next_frame()
    }
    /// True after a pull observes engine completion. Further pulls return None.
    pub fn is_finished(&self) -> bool {
        self.0.is_finished()
    }
}
