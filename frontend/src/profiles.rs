//! Shared path helper for on-disk state under `~/.config/lumen/`.
//!
//! Historically this module held a `Profile` struct + per-slider state
//! management. With the Lumen architecture (sliders live in the in-game
//! overlay, not the frontend window) the frontend no longer owns any of
//! that — all that remains is the XDG-compliant config-dir resolver,
//! used by the game-scanner state (`games.rs`) and any future frontend
//! state.

use std::path::PathBuf;

/// `$XDG_CONFIG_HOME/lumen` if set, else `$HOME/.config/lumen`, else
/// `/tmp/lumen` as a last-resort fallback (matches the layer-side
/// logic in `profile_manager.cpp::gffConfigRoot`).
pub fn config_dir() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::var_os("HOME")
                .map(|h| PathBuf::from(h).join(".config"))
                .unwrap_or_else(|| PathBuf::from("/tmp"))
        });
    base.join("lumen")
}
