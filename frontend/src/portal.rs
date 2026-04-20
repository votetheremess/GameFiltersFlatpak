use ashpd::desktop::global_shortcuts::{GlobalShortcuts, NewShortcut};
use ashpd::WindowIdentifier;
use futures_util::StreamExt;

use crate::ipc::Client;
use crate::messages::FrontendCommand;

/// Register the default overlay hotkey (Ctrl+Alt+F) via the XDG
/// GlobalShortcuts portal. On Bazzite KDE this routes through plasma-
/// shortcuts; on GNOME it falls back to nothing (unsupported portal).
pub fn register_hotkey(ipc: Client) {
    std::thread::Builder::new()
        .name("gff-portal".to_owned())
        .spawn(move || {
            log::info!("portal: thread started");
            let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
                Ok(rt) => rt,
                Err(e) => {
                    log::error!("portal: failed to build tokio runtime: {e}");
                    return;
                }
            };
            rt.block_on(async move {
                match run(ipc).await {
                    Ok(()) => log::info!("portal: run loop exited cleanly"),
                    Err(e) => log::warn!("portal: GlobalShortcuts unavailable: {e}"),
                }
            });
        })
        .expect("spawn portal thread");
}

async fn run(ipc: Client) -> ashpd::Result<()> {
    log::info!("portal: requesting GlobalShortcuts proxy");
    let portal = GlobalShortcuts::new().await?;
    log::info!("portal: creating session");
    let session = portal.create_session().await?;
    log::info!("portal: session ready, listing existing shortcuts");

    // Check if the shortcut is already bound from a previous run. If so,
    // reuse it — otherwise the portal prompts the user on every launch
    // (which is what you're seeing).
    let existing = match portal.list_shortcuts(&session).await?.response() {
        Ok(list) => list.shortcuts().to_vec(),
        Err(e) => {
            log::debug!("portal: list_shortcuts returned error (first-run): {e:?}");
            Vec::new()
        }
    };
    let already_bound = existing.iter().any(|s| s.id() == "toggle-overlay");
    log::info!(
        "portal: existing shortcuts = {}, already_bound = {already_bound}",
        existing.len()
    );

    if !already_bound {
        log::info!("portal: binding new shortcut toggle-overlay (Ctrl+Alt+F)");
        let shortcut = NewShortcut::new("toggle-overlay", "Toggle Nvidia Filters overlay")
            .preferred_trigger(Some("CTRL+ALT+f"));
        let parent = WindowIdentifier::default();
        portal.bind_shortcuts(&session, &[shortcut], &parent).await?.response()?;
        log::info!("portal: bind_shortcuts completed");
    }

    log::info!("portal: subscribing to activation stream");
    let mut activated = portal.receive_activated().await?;
    while let Some(evt) = activated.next().await {
        log::info!("portal: received activation for shortcut id = {:?}", evt.shortcut_id());
        if evt.shortcut_id() == "toggle-overlay" {
            ipc.send(FrontendCommand::ToggleOverlay);
        }
    }
    log::warn!("portal: activation stream ended");
    Ok(())
}
