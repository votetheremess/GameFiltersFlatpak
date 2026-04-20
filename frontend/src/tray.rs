use std::sync::mpsc;

use ksni::{menu, Tray, TrayService};

/// Commands sent from the tray thread back to the GTK main thread.
/// GTK / libadwaita objects are not `Send`, so we cannot touch them
/// directly from the tray thread — we marshal intent through this enum
/// and `app.rs` dispatches on the main thread.
#[derive(Debug, Clone, Copy)]
pub enum TrayCommand {
    ShowOverlay,
    HideOverlay,
    ShowWindow,
    Quit,
}

struct NffTray {
    tx: mpsc::Sender<TrayCommand>,
}

impl Tray for NffTray {
    fn id(&self) -> String {
        "com.gamefiltersflatpak.App".into()
    }

    fn title(&self) -> String {
        "Nvidia Filters".into()
    }

    fn icon_name(&self) -> String {
        "video-display-symbolic".into()
    }

    fn tool_tip(&self) -> ksni::ToolTip {
        ksni::ToolTip {
            icon_name: "video-display-symbolic".into(),
            title: "Nvidia Filters".into(),
            description: "Click to show window".into(),
            icon_pixmap: vec![],
        }
    }

    fn activate(&mut self, _x: i32, _y: i32) {
        let _ = self.tx.send(TrayCommand::ShowWindow);
    }

    fn menu(&self) -> Vec<menu::MenuItem<Self>> {
        use menu::*;
        vec![
            StandardItem {
                label: "Show Overlay".into(),
                activate: Box::new(|t: &mut Self| {
                    let _ = t.tx.send(TrayCommand::ShowOverlay);
                }),
                ..Default::default()
            }
            .into(),
            StandardItem {
                label: "Hide Overlay".into(),
                activate: Box::new(|t: &mut Self| {
                    let _ = t.tx.send(TrayCommand::HideOverlay);
                }),
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            StandardItem {
                label: "Show Window".into(),
                icon_name: "view-restore".into(),
                activate: Box::new(|t: &mut Self| {
                    let _ = t.tx.send(TrayCommand::ShowWindow);
                }),
                ..Default::default()
            }
            .into(),
            StandardItem {
                label: "Quit".into(),
                icon_name: "application-exit".into(),
                activate: Box::new(|t: &mut Self| {
                    let _ = t.tx.send(TrayCommand::Quit);
                }),
                ..Default::default()
            }
            .into(),
        ]
    }
}

/// Spawn the tray service on its own thread; the returned `Receiver`
/// is drained on the GTK main thread via a `glib::timeout_add_local`.
pub fn spawn() -> mpsc::Receiver<TrayCommand> {
    let (tx, rx) = mpsc::channel();
    let service = TrayService::new(NffTray { tx });
    service.spawn();
    log::info!("tray service spawned");
    rx
}
