use std::cell::RefCell;
use std::rc::Rc;

use adw::prelude::*;

use crate::ipc::Client;
use crate::messages::{FrontendCommand, ParamValue};
use crate::profiles::{self, Profile};

const STYLE_CSS: &str = "
window {
  font-size: 110%;
}
.gff-card {
  background-color: alpha(@window_bg_color, 0.92);
  border-radius: 8px;
  padding: 12px 16px;
  border: 1px solid alpha(white, 0.12);
}
.gff-section-header {
  font-weight: 700;
  opacity: 0.9;
  margin-bottom: 4px;
}
.gff-slider trough highlight {
  background-color: rgba(118, 195, 255, 0.55);
  border-radius: 6px;
}
.gff-slider slider {
  background-color: rgba(118, 195, 255, 0.95);
  border: 1px solid alpha(black, 0.2);
  border-radius: 10px;
}
";

pub fn build(app: &adw::Application, ipc: Client) -> adw::ApplicationWindow {
    ensure_css();

    let window = adw::ApplicationWindow::builder()
        .application(app)
        .title("Nvidia Filters")
        .default_width(440)
        .default_height(680)
        .build();

    let profile = Rc::new(RefCell::new(Profile {
        name: "default".into(),
        contrast: 1.0,
        gamma: 1.0,
        ..Profile::default()
    }));

    // Seed active.conf so the layer sees our effect chain and starts applying
    // the filter pipeline. Sliders write to this same path on every change.
    if let Err(e) = profiles::write_active(&profile.borrow()) {
        log::warn!("failed to seed active profile: {e}");
    }
    ipc.send(FrontendCommand::LoadProfile {
        path: profiles::active_config_path().to_string_lossy().into_owned(),
    });

    let toolbar = adw::ToolbarView::new();
    toolbar.add_top_bar(&adw::HeaderBar::new());

    let scroller = gtk::ScrolledWindow::new();
    scroller.set_hscrollbar_policy(gtk::PolicyType::Never);
    scroller.set_vexpand(true);

    let page = gtk::Box::new(gtk::Orientation::Vertical, 16);
    page.set_margin_top(16);
    page.set_margin_bottom(16);
    page.set_margin_start(16);
    page.set_margin_end(16);

    page.append(&slider_card(
        &ipc,
        &profile,
        "Exposure & Contrast",
        &[
            row("Brightness", "gff.brightness", -1.0, 1.0, 0.0, |p, v| p.brightness = v),
            row("Contrast", "gff.contrast", 0.0, 2.0, 1.0, |p, v| p.contrast = v),
            row("Highlights", "gff.highlights", -1.0, 1.0, 0.0, |p, v| p.highlights = v),
            row("Shadows", "gff.shadows", -1.0, 1.0, 0.0, |p, v| p.shadows = v),
        ],
    ));

    page.append(&slider_card(
        &ipc,
        &profile,
        "Color",
        &[
            row("Temperature", "gff.temperature", -1.0, 1.0, 0.0, |p, v| p.temperature = v),
            row("Vibrance", "gff.vibrance", -1.0, 1.0, 0.0, |p, v| p.vibrance = v),
        ],
    ));

    page.append(&slider_card(
        &ipc,
        &profile,
        "Details",
        &[
            row("Sharpen", "gff.sharpen", 0.0, 2.0, 0.0, |p, v| p.sharpen = v),
            row("Gamma", "gff.gamma", 0.5, 2.5, 1.0, |p, v| p.gamma = v),
            row("HDR Toning", "gff.hdrToning", 0.0, 1.0, 0.0, |p, v| p.hdr_toning = v),
        ],
    ));

    scroller.set_child(Some(&page));
    toolbar.set_content(Some(&scroller));
    window.set_content(Some(&toolbar));

    window
}

struct Row {
    label: &'static str,
    key: &'static str,
    min: f32,
    max: f32,
    initial: f32,
    apply: fn(&mut Profile, f32),
}

fn row(
    label: &'static str,
    key: &'static str,
    min: f32,
    max: f32,
    initial: f32,
    apply: fn(&mut Profile, f32),
) -> Row {
    Row { label, key, min, max, initial, apply }
}

fn ensure_css() {
    use std::sync::OnceLock;
    static LOADED: OnceLock<()> = OnceLock::new();
    LOADED.get_or_init(|| {
        let provider = gtk::CssProvider::new();
        provider.load_from_string(STYLE_CSS);
        if let Some(display) = gtk::gdk::Display::default() {
            gtk::style_context_add_provider_for_display(
                &display,
                &provider,
                gtk::STYLE_PROVIDER_PRIORITY_APPLICATION,
            );
        }
    });
}

fn slider_card(
    ipc: &Client,
    profile: &Rc<RefCell<Profile>>,
    title: &str,
    rows: &[Row],
) -> gtk::Box {
    let card = gtk::Box::new(gtk::Orientation::Vertical, 10);
    card.add_css_class("gff-card");

    let header = gtk::Label::new(Some(title));
    header.set_xalign(0.0);
    header.add_css_class("gff-section-header");
    card.append(&header);

    for r in rows {
        card.append(&slider_row(ipc, profile, r));
    }
    card
}

fn slider_row(ipc: &Client, profile: &Rc<RefCell<Profile>>, r: &Row) -> gtk::Box {
    let container = gtk::Box::new(gtk::Orientation::Horizontal, 12);

    let name = gtk::Label::new(Some(r.label));
    name.set_xalign(0.0);
    name.set_width_chars(12);
    container.append(&name);

    let step = (f64::from(r.max - r.min).abs() / 200.0).max(0.001);
    let scale = gtk::Scale::with_range(gtk::Orientation::Horizontal, f64::from(r.min), f64::from(r.max), step);
    scale.set_hexpand(true);
    scale.set_value(f64::from(r.initial));
    scale.set_draw_value(true);
    scale.set_value_pos(gtk::PositionType::Right);
    scale.add_css_class("gff-slider");

    let ipc = ipc.clone();
    let profile = profile.clone();
    let key = r.key.to_owned();
    let apply = r.apply;
    scale.connect_value_changed(move |s| {
        let v = s.value() as f32;
        {
            let mut p = profile.borrow_mut();
            apply(&mut p, v);
        }
        // Write the file so the layer's hot-reload picks it up (works even
        // if the game process loaded the layer in a different PID than the
        // frontend). Also notify over IPC for faster-than-hot-reload latency.
        if let Err(e) = profiles::write_active(&profile.borrow()) {
            log::warn!("write_active failed: {e}");
        }
        ipc.send(FrontendCommand::ParamUpdated {
            key: key.clone(),
            value: ParamValue::Float(v),
        });
    });

    container.append(&scale);
    container
}
