use serde::{Deserialize, Serialize};

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
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "type", rename_all = "kebab-case")]
pub enum FrontendCommand {
    ShowOverlay,
    HideOverlay,
    ToggleOverlay,
    LoadProfile { path: String },
    ParamUpdated { key: String, value: ParamValue },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
pub enum ParamValue {
    Bool(bool),
    Float(f32),
    Vec(Vec<f32>),
}
