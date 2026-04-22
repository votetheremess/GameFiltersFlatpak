use adw::prelude::*;

mod app;
mod games;
mod ipc;
mod messages;
mod portal;
mod profiles;
mod tray;
mod window;

const APP_ID: &str = "io.github.votetheremess.Lumen";

fn main() -> adw::glib::ExitCode {
    // Disable our own Vulkan layer inside this process. GTK4 uses Vulkan for
    // rendering, which would otherwise load our layer into the frontend and
    // let it win the IPC socket bind() — but the frontend has no game to draw
    // the overlay on, so toggle-overlay commands would be silently swallowed
    // here instead of reaching the game's layer instance. The env var must be
    // set BEFORE any Vulkan / GTK call, so we do it first thing in main().
    // SAFETY: we're single-threaded here; no other thread is reading env.
    unsafe { std::env::set_var("LUMEN_DISABLE", "1"); }

    // Configure env_logger to write unbuffered to stderr. The run.sh script
    // redirects stderr to LUMEN_LOG_FILE via shell redirection, so Rust logs
    // end up interleaved with layer output correctly without any in-process
    // buffering (the previous Target::Pipe approach buffered messages until
    // the process exited, making live debugging impossible).
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_millis()
        .init();

    log::info!("main() — frontend pid {}", std::process::id());

    // KDE writes `gtk-application-prefer-dark-theme=true` to
    // ~/.config/gtk-4.0/settings.ini for system-wide GTK dark mode. That
    // GtkSettings property is incompatible with libadwaita (which emits a
    // one-shot warning when it sees any non-default value on startup).
    // We want dark regardless — so after gtk_init loads settings.ini we
    // reset the property, then `app::wire` locks AdwStyleManager to
    // ForceDark. User's settings.ini is left untouched.
    gtk::init().expect("gtk init");
    if let Some(settings) = gtk::Settings::default() {
        settings.set_gtk_application_prefer_dark_theme(false);
    }

    let application = adw::Application::builder().application_id(APP_ID).build();
    app::wire(&application);
    application.run_with_args::<&str>(&[])
}
