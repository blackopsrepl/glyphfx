//! Private engine ABI and its synchronized ownership boundary.
use std::{
    ffi::{c_char, CStr, CString},
    marker::PhantomData,
    ptr::NonNull,
    rc::Rc,
    sync::{Mutex, MutexGuard},
};

static ENGINE: Mutex<()> = Mutex::new(());
fn lock() -> MutexGuard<'static, ()> {
    ENGINE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[repr(C)]
struct RawClip {
    _opaque: [u8; 0],
}
extern "C" {
    fn glyphfx_open(
        name: *const c_char,
        text: *const c_char,
        width: u16,
        height: u16,
        seed: u64,
        error: *mut c_char,
        error_len: usize,
    ) -> *mut RawClip;
    fn glyphfx_next(clip: *mut RawClip) -> *const c_char;
    fn glyphfx_close(clip: *mut RawClip);
}

/// Owned engine context. All C calls share a process-wide lock protecting the
/// render log, intern tables and visual pool. RNG and render logs are per clip.
/// Thread affinity is conservative; distinct clips can be used on distinct threads.
pub struct Session {
    raw: NonNull<RawClip>,
    finished: bool,
    _affinity: PhantomData<Rc<()>>,
}

impl Session {
    pub fn new(
        effect: &str,
        text: &str,
        width: u16,
        height: u16,
        seed: u64,
    ) -> Result<Self, String> {
        if width == 0 || height == 0 || u32::from(width) * u32::from(height) > 65_536 {
            return Err("canvas must be nonzero and at most 65536 cells".into());
        }
        if text.len() > 1_048_576 {
            return Err("input exceeds 1 MiB".into());
        }
        if text.trim().is_empty() {
            return Err("input must contain visible text".into());
        }
        let effect = CString::new(effect).map_err(|_| "effect contains NUL")?;
        let text = CString::new(text).map_err(|_| "text contains NUL")?;
        let mut error = [0 as c_char; 512];
        let _guard = lock();
        // SAFETY: all pointers reference live NUL-terminated storage; dimensions
        // are bounded above. C returns an owned context or NULL and writes at
        // most error.len() bytes to the initialized error buffer.
        let raw = unsafe {
            glyphfx_open(
                effect.as_ptr(),
                text.as_ptr(),
                width,
                height,
                seed,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        let raw = NonNull::new(raw).ok_or_else(|| {
            // SAFETY: the buffer is zero initialized and C uses snprintf.
            let detail = unsafe { CStr::from_ptr(error.as_ptr()) }.to_string_lossy();
            if detail.is_empty() {
                "engine allocation failed".into()
            } else {
                detail.into_owned()
            }
        })?;
        Ok(Self {
            raw,
            finished: false,
            _affinity: PhantomData,
        })
    }

    pub fn next_frame(&mut self) -> Option<String> {
        if self.finished {
            return None;
        }
        let _guard = lock();
        // SAFETY: self owns the live context; exclusive borrow and global lock
        // exclude mutation. Copy C's borrowed frame before releasing the lock.
        let frame = unsafe { glyphfx_next(self.raw.as_ptr()) };
        if frame.is_null() {
            self.finished = true;
            None
        } else {
            Some(
                unsafe { CStr::from_ptr(frame) }
                    .to_string_lossy()
                    .into_owned(),
            )
        }
    }

    pub fn is_finished(&self) -> bool {
        self.finished
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _guard = lock();
        // SAFETY: exactly one owner, never previously freed; lock covers globals.
        unsafe { glyphfx_close(self.raw.as_ptr()) }
    }
}
