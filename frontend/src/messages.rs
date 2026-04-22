use serde::{Deserialize, Serialize};

use crate::games::GameId;

/// Messages the layer sends to the frontend.
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "type", rename_all = "kebab-case")]
pub enum LayerEvent {
    GameStarted { pid: u32, exe: String },
    GameStopped { pid: u32 },
    OverlayShown,
    OverlayHidden,
}

/// Messages the frontend sends to the layer.
///
/// `LoadProfile` and `ParamUpdated` remain as variants for protocol
/// compatibility — the layer accepts them as no-ops. Nothing on the
/// frontend side emits them anymore (sliders moved into the in-game
/// overlay); they'll be deleted from both sides in a later cleanup pass
/// once we're sure no external caller depends on the schema.
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "type", rename_all = "kebab-case")]
pub enum FrontendCommand {
    ShowOverlay,
    HideOverlay,
    ToggleOverlay,
    LoadProfile { path: String },
    ParamUpdated { key: String, value: ParamValue },
    /// Full snapshot of the games the user has toggled ON in the
    /// frontend. Broadcast on every client connect and on every toggle
    /// change; layer caches locally and consults it at
    /// `vkCreateSwapchainKHR` (via `lumen::isGameEnabled()`) to decide
    /// whether to install the full effect pipeline or fall through to
    /// pass-through bypass.
    GamesEnabledUpdate { enabled: Vec<GameId> },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
pub enum ParamValue {
    Bool(bool),
    Float(f32),
    Vec(Vec<f32>),
}
