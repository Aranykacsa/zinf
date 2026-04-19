/// Hand-written FFI bindings to the ZINF C library.
/// Matches api.h, config.h, and linux_driver.h.
use std::os::raw::c_char;

/// Opaque handle — Rust never reads the fields; C owns the storage.
#[repr(C)]
pub struct ZinfCtx {
    _opaque: [u8; 0],
    _marker: std::marker::PhantomData<(*mut u8, std::marker::PhantomPinned)>,
}

/// Matches zinf_scrub_report_t in api.h
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct ScrubReport {
    pub checked: u32,
    pub healthy: u32,
    pub repaired: u32,
    pub unrecoverable: u32,
}

/// Return code constants (from config.h)
pub const STORAGE_OK: u8 = 0;

#[allow(dead_code)]
extern "C" {
    /// Global context pointer defined by platform_linux.c
    pub static mut zinf_ctx: *mut ZinfCtx;

    /// Set the block device path used by linux_driver (default: /dev/loop0)
    pub fn linux_driver_set_path(path: *const c_char);

    /// Initialise driver + storage
    pub fn setup_storage(ctx: *mut ZinfCtx) -> u8;

    /// Initialise the metadata (log) sector
    pub fn init_log_sector(ctx: *mut ZinfCtx) -> u8;

    /// Read one logical sector via RAID (with CRC verification & majority voting)
    pub fn raid_read(ctx: *mut ZinfCtx, logical_sector: u64, payload: *mut u8) -> u8;

    /// Return the LBA of the last written sector
    pub fn get_last_sector(ctx: *mut ZinfCtx, last_sector: *mut u64) -> u8;

    /// Iterate [start..end] inclusive, check & repair mirrors; fills *report
    pub fn zinf_scrub(
        ctx: *mut ZinfCtx,
        start: u64,
        end: u64,
        report: *mut ScrubReport,
    ) -> u8;
}
