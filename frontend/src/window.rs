//! Lumen frontend window — Nvidia-App-style game library scanner.
//!
//! Two sections:
//!   1. **Scan Locations** — Steam library paths + user-added directories.
//!      Each row has a remove button; "Add Location…" opens a directory
//!      picker (FileChooserNative) for adding extra Steam libraries or
//!      non-Steam game directories.
//!   2. **Games** — AdwSwitchRow per detected game; toggling updates both
//!      local state (persisted to `games.json`) and the IPC broadcast to
//!      connected layers. Default: all off. Header suffix has a "Scan Now"
//!      button that re-walks the scan locations.
//!
//! State threading: `Rc<RefCell<GamesState>>` is passed in from `app.rs` and
//! shared between every widget callback. Any mutation rebuilds the games
//! group from state and pushes the new enabled list to the IPC client.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use adw::prelude::*;

use crate::games::{self, DetectedGame, GamesState, LocationKind, ScanLocation, Source};
use crate::ipc::Client;

/// Shared-between-closures state the UI mutates in response to user
/// interaction. Held in `Rc<RefCell<...>>` — GTK callbacks run on the
/// main thread so no lock contention.
struct Ctx {
    state: Rc<RefCell<GamesState>>,
    ipc: Client,
    /// The AdwPreferencesGroup holding scan-location rows. Rebuilt when
    /// locations change.
    locations_group: adw::PreferencesGroup,
    /// The group holding game-toggle rows. Rebuilt when games change.
    games_group: adw::PreferencesGroup,
    /// Window handle — needed to parent FileChooserNative dialogs.
    window: adw::ApplicationWindow,
    /// The sentinel "Add Location…" row at the bottom of the locations
    /// group. Kept as a separate handle so it doesn't get removed during
    /// a rebuild (it's always the last row).
    add_location_row: adw::ActionRow,
    /// Remembered to avoid rebuilding the children of the Games group if
    /// nothing changed — lightweight, not strictly necessary.
    rebuilding: RefCell<bool>,
}

pub fn build(
    app: &adw::Application,
    ipc: Client,
    state: Rc<RefCell<GamesState>>,
) -> adw::ApplicationWindow {
    let window = adw::ApplicationWindow::builder()
        .application(app)
        .title("Lumen")
        .default_width(560)
        .default_height(760)
        .build();

    let toolbar = adw::ToolbarView::new();
    let header = adw::HeaderBar::new();

    // "Scan Now" button in the header — quick re-scan without needing
    // to touch the locations list.
    let scan_btn = gtk::Button::from_icon_name("view-refresh-symbolic");
    scan_btn.set_tooltip_text(Some("Scan locations for games"));
    header.pack_end(&scan_btn);
    toolbar.add_top_bar(&header);

    let scroller = gtk::ScrolledWindow::new();
    scroller.set_hscrollbar_policy(gtk::PolicyType::Never);
    scroller.set_vexpand(true);

    let page = adw::PreferencesPage::new();

    // --- Locations group ---
    let locations_group = adw::PreferencesGroup::new();
    locations_group.set_title("Scan Locations");
    locations_group.set_description(Some(
        "Where Lumen looks for games. Remove defaults you don't use; \
         add paths to other Steam libraries or non-Steam game folders.",
    ));

    let add_location_row = adw::ActionRow::new();
    add_location_row.set_title("Add Location…");
    add_location_row.set_activatable(true);
    let add_icon = gtk::Image::from_icon_name("list-add-symbolic");
    add_location_row.add_suffix(&add_icon);
    locations_group.add(&add_location_row);

    page.add(&locations_group);

    // --- Games group ---
    let games_group = adw::PreferencesGroup::new();
    games_group.set_title("Games");
    games_group.set_description(Some(
        "Toggle on the games you want Lumen's overlay to activate for. \
         Changes take effect the next time the game launches.",
    ));
    page.add(&games_group);

    scroller.set_child(Some(&page));
    toolbar.set_content(Some(&scroller));
    window.set_content(Some(&toolbar));

    // Shared context for all the closure plumbing below.
    let ctx = Rc::new(Ctx {
        state,
        ipc,
        locations_group: locations_group.clone(),
        games_group: games_group.clone(),
        window: window.clone(),
        add_location_row: add_location_row.clone(),
        rebuilding: RefCell::new(false),
    });

    // Wire the Add Location row — opens a directory picker.
    {
        let ctx = ctx.clone();
        add_location_row.connect_activated(move |_| open_add_location_dialog(ctx.clone()));
    }

    // Wire Scan Now — rescan + merge (preserves enabled state).
    {
        let ctx = ctx.clone();
        scan_btn.connect_clicked(move |_| rescan_and_rebuild(&ctx));
    }

    // Initial render of both groups from loaded state.
    rebuild_locations(&ctx);
    rebuild_games(&ctx);

    window
}

// ============================================================================
// Rebuild helpers — rip all rows out of the group and repopulate from state
// ============================================================================

fn rebuild_locations(ctx: &Ctx) {
    // Strip existing location rows (everything EXCEPT the sentinel add row).
    let mut child = ctx.locations_group.first_child();
    while let Some(c) = child {
        let next = c.next_sibling();
        // The AdwPreferencesGroup has an internal ListBox — iterating
        // first_child/next_sibling walks ListBoxRow widgets. We identify
        // our sentinel by widget identity (not text) since the user
        // could have a location row with the same title.
        if !c.is_ancestor(&ctx.add_location_row) && c.upcast_ref::<gtk::Widget>() != ctx.add_location_row.upcast_ref::<gtk::Widget>() {
            // Our `add_location_row` may be nested inside a ListBoxRow;
            // test by walking up.
            let mut is_add = false;
            let mut w: Option<gtk::Widget> = Some(c.clone());
            while let Some(cur) = w {
                if cur.upcast_ref::<gtk::Widget>() == ctx.add_location_row.upcast_ref::<gtk::Widget>() {
                    is_add = true;
                    break;
                }
                w = cur.first_child();
            }
            if !is_add {
                ctx.locations_group.remove(&c);
            }
        }
        child = next;
    }

    // Re-add a row for each current scan location, before the sentinel.
    let locs: Vec<ScanLocation> = ctx.state.borrow().scan_locations.clone();
    for loc in locs.iter() {
        let row = build_location_row(ctx, loc);
        // GTK4 libadwaita: `add` appends at the end; we want the sentinel
        // to stay last, so we remove the sentinel, add the new row, then
        // re-add the sentinel. Small churn but correct.
        ctx.locations_group.remove(&ctx.add_location_row);
        ctx.locations_group.add(&row);
        ctx.locations_group.add(&ctx.add_location_row);
    }
}

fn build_location_row(ctx: &Ctx, loc: &ScanLocation) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(&glib::markup_escape_text(&loc.path.to_string_lossy()));
    let kind_str = match loc.kind {
        LocationKind::Steam => "Steam library",
        LocationKind::Manual => "Custom directory",
    };
    let badge = if loc.added_by_user { "user-added" } else { "default" };
    row.set_subtitle(&format!("{kind_str} · {badge}"));

    let remove_btn = gtk::Button::from_icon_name("edit-delete-symbolic");
    remove_btn.add_css_class("flat");
    remove_btn.set_valign(gtk::Align::Center);
    remove_btn.set_tooltip_text(Some("Remove this scan location"));
    {
        let ctx = Rc::new(ctx_clone(ctx));
        let loc_path = loc.path.clone();
        remove_btn.connect_clicked(move |_| {
            ctx.state
                .borrow_mut()
                .scan_locations
                .retain(|l| l.path != loc_path);
            save_and_rebuild(&ctx);
        });
    }
    row.add_suffix(&remove_btn);

    row
}

fn rebuild_games(ctx: &Ctx) {
    // Clear all children of the games group.
    let mut child = ctx.games_group.first_child();
    while let Some(c) = child {
        let next = c.next_sibling();
        ctx.games_group.remove(&c);
        child = next;
    }

    let games: Vec<DetectedGame> = ctx.state.borrow().games.clone();
    if games.is_empty() {
        let empty = adw::ActionRow::new();
        empty.set_title("No games detected");
        empty.set_subtitle("Add a scan location or click Scan Now.");
        empty.set_sensitive(false);
        ctx.games_group.add(&empty);
        return;
    }

    for (idx, game) in games.iter().enumerate() {
        let row = adw::SwitchRow::new();
        row.set_title(&glib::markup_escape_text(&game.display_name));
        let source_label = match &game.source {
            Source::Steam { app_id, .. } => format!("Steam · appid {app_id}"),
            Source::Manual { .. } => "Custom".to_string(),
        };
        row.set_subtitle(&format!(
            "{source_label} · {}",
            glib::markup_escape_text(&game.install_path.to_string_lossy())
        ));
        row.set_active(game.enabled);

        // Row thumbnail (if Steam cached one). We pre-scale the source
        // image to exactly 48×48 (center-cropped to square first) so the
        // resulting Picture's natural size is 48×48 — GTK's layout uses
        // natural size to negotiate allocations, and without this step
        // Schedule I's 460×215 header would stretch the prefix column
        // wider than the other rows' square icons. `set_size_request`
        // alone doesn't help: it's a minimum, not a maximum, and a
        // `gtk::Box` with `set_overflow(Hidden)` clips painting but
        // doesn't shrink the allocation. Silently skipped if the file
        // is missing / unreadable.
        if let Some(icon_path) = &game.icon_path {
            if let Some(texture) = load_icon_thumbnail(icon_path, 48) {
                let picture = gtk::Picture::for_paintable(&texture);
                picture.set_size_request(48, 48);
                picture.set_halign(gtk::Align::Start);
                picture.set_valign(gtk::Align::Center);
                picture.set_overflow(gtk::Overflow::Hidden);
                picture.add_css_class("card");
                row.add_prefix(&picture);
            }
        }

        let ctx_cloned = Rc::new(ctx_clone(ctx));
        row.connect_active_notify(move |r| {
            // Guard against the programmatic `set_active` during rebuild
            // firing us back into an infinite save loop.
            if *ctx_cloned.rebuilding.borrow() {
                return;
            }
            let new_state = r.is_active();
            {
                let mut s = ctx_cloned.state.borrow_mut();
                if let Some(g) = s.games.get_mut(idx) {
                    if g.enabled == new_state {
                        return;
                    }
                    g.enabled = new_state;
                }
            }
            // Persist + broadcast the updated enabled list.
            if let Err(e) = games::save(&ctx_cloned.state.borrow()) {
                log::warn!("games.json save failed: {e}");
            }
            ctx_cloned
                .ipc
                .update_enabled_games(ctx_cloned.state.borrow().enabled_ids());
        });
        ctx.games_group.add(&row);
    }
}

fn rescan_and_rebuild(ctx: &Ctx) {
    let scanned = games::scan_all(&ctx.state.borrow().scan_locations);
    let merged = games::merge_scan(&ctx.state.borrow(), scanned);
    ctx.state.borrow_mut().games = merged;
    save_and_rebuild(ctx);
}

fn save_and_rebuild(ctx: &Ctx) {
    *ctx.rebuilding.borrow_mut() = true;
    if let Err(e) = games::save(&ctx.state.borrow()) {
        log::warn!("games.json save failed: {e}");
    }
    ctx.ipc
        .update_enabled_games(ctx.state.borrow().enabled_ids());
    rebuild_locations(ctx);
    rebuild_games(ctx);
    *ctx.rebuilding.borrow_mut() = false;
}

// ============================================================================
// Add Location dialog
// ============================================================================

fn open_add_location_dialog(ctx: Rc<Ctx>) {
    let dialog = gtk::FileChooserNative::new(
        Some("Add Scan Location"),
        Some(&ctx.window),
        gtk::FileChooserAction::SelectFolder,
        Some("Select"),
        Some("Cancel"),
    );
    dialog.set_modal(true);
    let ctx_for_response = ctx.clone();
    dialog.connect_response(move |d, response| {
        if response == gtk::ResponseType::Accept {
            if let Some(file) = d.file() {
                if let Some(path) = file.path() {
                    add_location(&ctx_for_response, &path);
                }
            }
        }
        d.destroy();
    });
    dialog.show();
}

fn add_location(ctx: &Ctx, path: &Path) {
    // Guess whether this is a Steam library (contains `libraryfolders.vdf`
    // or `appmanifest_*.acf`) or a manual directory. Keeps the badge
    // accurate and lets scan_all dispatch to the right backend.
    let kind = if is_steam_library(path) {
        LocationKind::Steam
    } else {
        LocationKind::Manual
    };
    let new_loc = ScanLocation {
        path: path.to_path_buf(),
        kind,
        added_by_user: true,
    };
    // De-duplicate: if the user added the same path twice, do nothing.
    {
        let mut s = ctx.state.borrow_mut();
        if s.scan_locations.iter().any(|l| l.path == new_loc.path) {
            return;
        }
        s.scan_locations.push(new_loc);
    }
    rescan_and_rebuild(ctx);
}

fn is_steam_library(path: &Path) -> bool {
    // A steamapps dir contains libraryfolders.vdf OR at least one
    // appmanifest_*.acf. If neither is present, assume it's a manual
    // games directory.
    if path.join("libraryfolders.vdf").is_file() {
        return true;
    }
    if let Ok(entries) = std::fs::read_dir(path) {
        for e in entries.flatten() {
            let name = e.file_name();
            let n = name.to_string_lossy();
            if n.starts_with("appmanifest_") && n.ends_with(".acf") {
                return true;
            }
        }
    }
    false
}

// ============================================================================
// Helpers
// ============================================================================

/// Manual Rc-less Ctx clone (we can't derive Clone on Ctx — gtk types
/// inside don't all derive Clone, but their Rc::clone works).  We just
/// clone the Rc-wrapped parts and the widget handles (all GObjects,
/// cheap ref-count bumps).
fn ctx_clone(ctx: &Ctx) -> Ctx {
    Ctx {
        state: Rc::clone(&ctx.state),
        ipc: ctx.ipc.clone(),
        locations_group: ctx.locations_group.clone(),
        games_group: ctx.games_group.clone(),
        window: ctx.window.clone(),
        add_location_row: ctx.add_location_row.clone(),
        rebuilding: RefCell::new(*ctx.rebuilding.borrow()),
    }
}

use adw::glib;

/// Load a Steam-cache image and reduce it to an exact `size`×`size`
/// square texture. Center-crops first so wide banners (like Steam's
/// 460×215 `header.jpg`) don't get squashed, then scales the resulting
/// square down. Returns `None` on any IO / decode failure; caller
/// silently skips the prefix widget in that case.
fn load_icon_thumbnail(path: &std::path::Path, size: i32) -> Option<gtk::gdk::Texture> {
    let pixbuf = gtk::gdk_pixbuf::Pixbuf::from_file(path).ok()?;
    let w = pixbuf.width();
    let h = pixbuf.height();
    let side = w.min(h).max(1);
    let x = ((w - side) / 2).max(0);
    let y = ((h - side) / 2).max(0);
    let cropped = pixbuf.new_subpixbuf(x, y, side, side);
    let scaled = cropped.scale_simple(size, size, gtk::gdk_pixbuf::InterpType::Bilinear)?;
    Some(gtk::gdk::Texture::for_pixbuf(&scaled))
}
