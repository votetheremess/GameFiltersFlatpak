use std::cell::Cell;
use std::rc::Rc;
use std::time::Duration;

use adw::glib;
use adw::prelude::*;

use crate::ipc::Client;
use crate::messages::FrontendCommand;
use crate::tray::{self, TrayCommand};
use crate::{portal, window};

pub fn wire(app: &adw::Application) {
    let setup_done = Rc::new(Cell::new(false));

    app.connect_startup(|_app| {
        log::info!("game-filters-flatpak startup");
        // Lock to dark the libadwaita-approved way. Setting the GTK4-era
        // `gtk-application-prefer-dark-theme` on GtkSettings emits a
        // deprecation warning and doesn't fully apply the Adwaita palette
        // (the color tokens come from AdwStyleManager, not GtkSettings).
        adw::StyleManager::default().set_color_scheme(adw::ColorScheme::ForceDark);
    });

    app.connect_activate({
        let setup_done = setup_done.clone();
        move |app| {
            if setup_done.replace(true) {
                if let Some(existing) = app.active_window() {
                    existing.present();
                }
                return;
            }

            let ipc_client = Client::spawn();
            portal::register_hotkey(ipc_client.clone());

            let win = window::build(app, ipc_client.clone());

            // Hide on close, keep the daemon (tray + IPC + portal hotkey)
            // running in the background. Explicit Quit is via the tray menu.
            // `app.hold()` keeps GApplication alive even when zero windows
            // are visible, so GTK's "last window closed → quit" doesn't fire.
            app.hold();
            win.connect_close_request(|w| {
                w.set_visible(false);
                glib::Propagation::Stop
            });

            win.present();

            // Tray runs on its own thread (ksni requires Send); it signals
            // back through an mpsc channel which we drain on the GTK main
            // loop so we can safely touch GTK/libadwaita objects.
            let tray_rx = tray::spawn();
            let app_for_timer = app.clone();
            let win_for_timer = win.clone();
            let ipc_for_timer = ipc_client.clone();
            glib::timeout_add_local(Duration::from_millis(100), move || {
                while let Ok(cmd) = tray_rx.try_recv() {
                    match cmd {
                        TrayCommand::ShowOverlay => ipc_for_timer.send(FrontendCommand::ShowOverlay),
                        TrayCommand::HideOverlay => ipc_for_timer.send(FrontendCommand::HideOverlay),
                        TrayCommand::ShowWindow => win_for_timer.present(),
                        TrayCommand::Quit => {
                            app_for_timer.quit();
                            return glib::ControlFlow::Break;
                        }
                    }
                }
                glib::ControlFlow::Continue
            });
        }
    });
}
