//! Game library scanner + enabled-games state.
//!
//! Models the frontend's "Nvidia-App-style" library view: a list of scan
//! locations (Steam libraries + user-added dirs) and a list of detected
//! games with per-game enable toggles. Persists to
//! `~/.config/lumen/games.json`. The enabled subset is pushed to all
//! connected layer clients over IPC (`GamesEnabledUpdate`); the layer
//! uses that list to decide whether to install its full effect pipeline
//! or fall through to pass-through bypass.
//!
//! Scanning strategy:
//!   - **Steam libraries**: parse `libraryfolders.vdf` to enumerate all
//!     library roots; for each, read every `appmanifest_<appid>.acf` and
//!     extract `appid`, `name`, `installdir`. Each manifest becomes one
//!     `DetectedGame { id: GameId::SteamApp(appid), ... }`.
//!   - **Manual dirs**: recursively walk the user-chosen directory up to
//!     depth 3, collect every `.exe`, and emit one `DetectedGame` per
//!     file with `id: GameId::Executable(path)`. Shallow depth keeps
//!     scans fast and avoids traversing e.g. the entire `$HOME`.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

// ============================================================================
// VDF (Valve Data Format) parser
// ============================================================================
//
// Minimal hand-rolled parser for the two file shapes we care about:
// `libraryfolders.vdf` and `appmanifest_<appid>.acf`. Both are quoted-string
// key-value trees with brace-delimited nested blocks. Whitespace between
// key and value is tabs in Steam's output — we accept any whitespace mix.
// Also handles `// ...` line comments (rare but valid per the informal
// Valve spec). Does not handle `#include` directives (those appear in
// a couple of Steam config files but never in libraryfolders/appmanifest).

#[derive(Debug, Clone)]
enum VdfValue {
    String(String),
    Block(BTreeMap<String, VdfValue>),
}

impl VdfValue {
    fn as_str(&self) -> Option<&str> {
        match self {
            VdfValue::String(s) => Some(s),
            _ => None,
        }
    }
    fn as_block(&self) -> Option<&BTreeMap<String, VdfValue>> {
        match self {
            VdfValue::Block(b) => Some(b),
            _ => None,
        }
    }
}

fn parse_vdf(input: &str) -> Option<BTreeMap<String, VdfValue>> {
    let bytes = input.as_bytes();
    let mut cursor = 0usize;
    let mut outer = BTreeMap::new();
    loop {
        if !skip_ws(bytes, &mut cursor) {
            break;
        }
        if cursor >= bytes.len() {
            break;
        }
        let key = parse_string(bytes, &mut cursor)?;
        skip_ws(bytes, &mut cursor);
        let value = parse_value(bytes, &mut cursor)?;
        outer.insert(key, value);
    }
    Some(outer)
}

/// Advance past whitespace and `//` line comments. Returns true if there's
/// still content to read, false at EOF.
fn skip_ws(bytes: &[u8], i: &mut usize) -> bool {
    while *i < bytes.len() {
        match bytes[*i] {
            b' ' | b'\t' | b'\n' | b'\r' => *i += 1,
            b'/' if *i + 1 < bytes.len() && bytes[*i + 1] == b'/' => {
                while *i < bytes.len() && bytes[*i] != b'\n' {
                    *i += 1;
                }
            }
            _ => return true,
        }
    }
    false
}

/// Parse a double-quoted string starting at bytes[*i]. Handles `\"` and
/// `\\` escapes, advances cursor past the closing quote.
fn parse_string(bytes: &[u8], i: &mut usize) -> Option<String> {
    if *i >= bytes.len() || bytes[*i] != b'"' {
        return None;
    }
    *i += 1;
    let mut out = String::new();
    while *i < bytes.len() {
        let b = bytes[*i];
        if b == b'\\' && *i + 1 < bytes.len() {
            *i += 1;
            out.push(bytes[*i] as char);
            *i += 1;
        } else if b == b'"' {
            *i += 1;
            return Some(out);
        } else {
            out.push(b as char);
            *i += 1;
        }
    }
    None
}

fn parse_value(bytes: &[u8], i: &mut usize) -> Option<VdfValue> {
    if *i >= bytes.len() {
        return None;
    }
    if bytes[*i] == b'{' {
        *i += 1;
        let mut block = BTreeMap::new();
        loop {
            if !skip_ws(bytes, i) {
                return None;
            }
            if *i >= bytes.len() {
                return None;
            }
            if bytes[*i] == b'}' {
                *i += 1;
                return Some(VdfValue::Block(block));
            }
            let key = parse_string(bytes, i)?;
            skip_ws(bytes, i);
            let value = parse_value(bytes, i)?;
            block.insert(key, value);
        }
    } else if bytes[*i] == b'"' {
        parse_string(bytes, i).map(VdfValue::String)
    } else {
        None
    }
}

// ============================================================================
// Data model
// ============================================================================

/// Canonical identifier the layer uses to decide "is this process a game
/// the user toggled on?". Steam games match by `$SteamAppId` env var;
/// manual-dir games match by absolute exe path from `/proc/self/cmdline`.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq, Hash)]
#[serde(tag = "kind", content = "value")]
pub enum GameId {
    /// SteamAppId as string (Steam sets this env var uniformly for all
    /// Steam-launched processes including Proton helpers — toggling a
    /// game ON here enables the overlay for the whole appid).
    SteamApp(String),
    /// Absolute path to a specific `.exe` (for non-Steam games added via
    /// "Add Location"). Matched by the layer against the first NUL-
    /// terminated token of `/proc/self/cmdline`.
    Executable(PathBuf),
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
pub enum LocationKind {
    Steam,
    Manual,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScanLocation {
    pub path: PathBuf,
    pub kind: LocationKind,
    /// False for hardcoded defaults (populated by `default_scan_locations`),
    /// true for locations the user explicitly picked via the Add Location
    /// button. Distinguishes "this one comes back if I remove and rescan"
    /// from "my pick — don't resurrect unless I re-add."
    #[serde(default)]
    pub added_by_user: bool,
}

/// Where a detected game came from, carried in the JSON state so a rescan
/// can attribute games to their origin.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum Source {
    Steam {
        app_id: String,
        library_path: PathBuf,
    },
    Manual {
        root: PathBuf,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DetectedGame {
    pub id: GameId,
    pub display_name: String,
    pub install_path: PathBuf,
    /// Absolute path to a Steam-cached icon / header image (JPG or PNG),
    /// rendered as the row's prefix thumbnail. Steam stores these under
    /// `<Steam>/appcache/librarycache/<appid>/`. None for manual-dir
    /// entries (we don't fabricate icons for those) and for Steam games
    /// that haven't been launched recently enough for Steam to cache art.
    #[serde(default)]
    pub icon_path: Option<PathBuf>,
    pub source: Source,
    #[serde(default)]
    pub enabled: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct GamesState {
    pub scan_locations: Vec<ScanLocation>,
    pub games: Vec<DetectedGame>,
}

impl GamesState {
    /// Returns the GameId of every game with `enabled = true`. Shape the
    /// IPC broadcasts to layers.
    pub fn enabled_ids(&self) -> Vec<GameId> {
        self.games
            .iter()
            .filter(|g| g.enabled)
            .map(|g| g.id.clone())
            .collect()
    }
}

// ============================================================================
// Default scan locations (hardcoded)
// ============================================================================

/// The three canonical Steam install paths on Linux, filtered to only
/// those that actually exist on this machine. On Bazzite the primary
/// match is usually the Flatpak Steam path; on Fedora Workstation with
/// native Steam it's `~/.local/share/Steam`. `~/.steam/steam` is a
/// historical symlink most installs keep around.
pub fn default_scan_locations() -> Vec<ScanLocation> {
    let Some(home) = std::env::var_os("HOME").map(PathBuf::from) else {
        return Vec::new();
    };
    let candidates = [
        home.join(".local/share/Steam/steamapps"),
        home.join(".var/app/com.valvesoftware.Steam/data/Steam/steamapps"),
        home.join(".steam/steam/steamapps"),
    ];
    candidates
        .iter()
        .filter(|p| p.is_dir())
        .map(|p| ScanLocation {
            path: p.clone(),
            kind: LocationKind::Steam,
            added_by_user: false,
        })
        .collect()
}

// ============================================================================
// Scanning
// ============================================================================

pub fn scan_all(locations: &[ScanLocation]) -> Vec<DetectedGame> {
    let mut out = Vec::new();
    for loc in locations {
        match loc.kind {
            LocationKind::Steam => out.extend(scan_steam_library(&loc.path)),
            LocationKind::Manual => out.extend(scan_manual_dir(&loc.path)),
        }
    }
    // De-duplicate by GameId — a user's `~/.steam/steam/steamapps` is
    // usually a symlink to `~/.local/share/Steam/steamapps`, so both scan
    // locations would produce the same games. First occurrence wins.
    let mut seen: HashMap<GameId, ()> = HashMap::new();
    out.retain(|g| seen.insert(g.id.clone(), ()).is_none());
    out
}

/// Scan a Steam library at `steamapps_path`. Reads `libraryfolders.vdf`
/// to find all library roots (the user may have added external drives);
/// for each, enumerates `appmanifest_<appid>.acf` files and pulls out
/// the appid, display name, and install subdirectory. Skips Steam's own
/// runtime + compatibility tools (Proton, Linux Runtime, Steamworks
/// Redistributables) so the list only shows actual games.
pub fn scan_steam_library(steamapps_path: &Path) -> Vec<DetectedGame> {
    let mut games = Vec::new();
    let libs = read_libraryfolders(steamapps_path)
        .unwrap_or_else(|| vec![steamapps_path.to_path_buf()]);
    for lib in libs {
        let Ok(entries) = fs::read_dir(&lib) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if !name.starts_with("appmanifest_") || !name.ends_with(".acf") {
                continue;
            }
            let Ok(body) = fs::read_to_string(entry.path()) else {
                continue;
            };
            let Some(parsed) = parse_vdf(&body) else {
                continue;
            };
            let Some(state) = parsed.get("AppState").and_then(|v| v.as_block()) else {
                continue;
            };
            let Some(app_id) = state.get("appid").and_then(|v| v.as_str()) else {
                continue;
            };
            let app_id = app_id.to_string();
            let display_name = state
                .get("name")
                .and_then(|v| v.as_str())
                .unwrap_or(&app_id)
                .to_string();
            if is_steam_tool(&display_name, &app_id) {
                continue;
            }
            let installdir = state
                .get("installdir")
                .and_then(|v| v.as_str())
                .unwrap_or("");
            let install_path = lib.join("common").join(installdir);
            let icon_path = icon_path_for_steam(&app_id, &lib);
            games.push(DetectedGame {
                id: GameId::SteamApp(app_id.clone()),
                display_name,
                install_path,
                icon_path,
                source: Source::Steam {
                    app_id,
                    library_path: lib.clone(),
                },
                enabled: false,
            });
        }
    }
    games
}

/// Recognizes Steam's built-in runtime + compatibility tools by
/// display-name pattern. These live in the same library as games and
/// have their own `appmanifest_*.acf` entries, but the user doesn't
/// think of them as games and can't run an overlay on them.
///
/// Match rules (case-insensitive prefix / substring):
///   - "Proton " / "Proton-" — every Proton version
///   - "Steam Linux Runtime" — Scout, Soldier, Sniper, etc.
///   - "Steamworks Common Redistributables"
///   - anything containing "Redistributable" / "Dedicated Server"
///
/// Also filters by well-known tool appids as a backstop — the string
/// match already covers them, but appid matching survives future name
/// changes from Valve.
fn is_steam_tool(display_name: &str, app_id: &str) -> bool {
    const KNOWN_TOOL_APPIDS: &[&str] = &[
        "228980",   // Steamworks Common Redistributables
        "1070560",  // Steam Linux Runtime (Soldier)
        "1391110",  // Steam Linux Runtime (Soldier)
        "1493710",  // Proton Experimental
        "1628350",  // Steam Linux Runtime 3.0 (Sniper)
        "1887720",  // Proton 7.0
        "2180100",  // Proton Hotfix
        "2230260",  // Proton 8.0
        "2805730",  // Proton 9.0
        "3435470",  // Proton 10.0
    ];
    if KNOWN_TOOL_APPIDS.contains(&app_id) {
        return true;
    }
    let lower = display_name.to_lowercase();
    if lower.starts_with("proton ") || lower.starts_with("proton-") {
        return true;
    }
    if lower.starts_with("steam linux runtime") {
        return true;
    }
    if lower.contains("redistributable") || lower.contains("dedicated server") {
        return true;
    }
    if lower == "steamworks common redistributables" {
        return true;
    }
    false
}

/// Resolve the local path of a Steam-cached library image for this
/// appid, or None if Steam hasn't cached anything for this game. Caller
/// loads it into a `gtk::Picture` as the row's prefix thumbnail.
///
/// Precedence: named files first (`header.jpg` → `library_600x900.jpg`
/// → `logo.png` — the named files are what Steam's older cache layout
/// used and they're still populated for most games). Newer content-
/// addressed hash-named `.jpg` files are the fallback — we take the
/// first `.jpg` we find in the appid directory.
fn icon_path_for_steam(app_id: &str, library_path: &Path) -> Option<PathBuf> {
    // library_path is `<Steam>/steamapps`; Steam root is its parent;
    // librarycache lives at `<Steam>/appcache/librarycache`.
    let steam_root = library_path.parent()?;
    let appid_dir = steam_root.join("appcache/librarycache").join(app_id);
    if !appid_dir.is_dir() {
        return None;
    }
    for name in ["header.jpg", "library_600x900.jpg", "logo.png", "icon.jpg"] {
        let p = appid_dir.join(name);
        if p.is_file() {
            return Some(p);
        }
    }
    // Newer content-addressed layout — pick the first jpg/png in the dir.
    let entries = fs::read_dir(&appid_dir).ok()?;
    for entry in entries.flatten() {
        let p = entry.path();
        if !p.is_file() {
            continue;
        }
        let ext = p.extension().and_then(|s| s.to_str()).unwrap_or("");
        if ext.eq_ignore_ascii_case("jpg") || ext.eq_ignore_ascii_case("png") {
            return Some(p);
        }
    }
    None
}

/// Parse the user's `libraryfolders.vdf` to get every Steam library path.
/// Returns None if the file is missing or malformed; the caller falls
/// back to scanning just the steamapps_path it was given.
fn read_libraryfolders(steamapps_path: &Path) -> Option<Vec<PathBuf>> {
    let body = fs::read_to_string(steamapps_path.join("libraryfolders.vdf")).ok()?;
    let parsed = parse_vdf(&body)?;
    let root = parsed.get("libraryfolders")?.as_block()?;
    let mut out = Vec::new();
    for (_idx, v) in root {
        let block = match v.as_block() {
            Some(b) => b,
            None => continue,
        };
        let Some(p) = block.get("path").and_then(|v| v.as_str()) else {
            continue;
        };
        // The "path" field is the Steam ROOT, not steamapps. E.g.
        // "/var/home/admin/.local/share/Steam" — we need to append
        // "/steamapps" to get the appmanifest directory.
        let lib = PathBuf::from(p).join("steamapps");
        if lib.is_dir() {
            out.push(lib);
        }
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

/// Walk a user-added directory up to depth 3, collecting every `.exe` as
/// a separate game entry. Depth 3 is enough for typical Windows-game
/// layouts (e.g. `Root/Binaries/Win64/game.exe`) without crawling the
/// entire home dir. No attempt to distinguish "the real game" from
/// helpers like `ShaderCompileWorker.exe` — user picks which to enable.
pub fn scan_manual_dir(root: &Path) -> Vec<DetectedGame> {
    let mut games = Vec::new();
    walk_exes(root, 3, &mut |exe_path| {
        let display_name = exe_path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("(unknown)")
            .to_string();
        games.push(DetectedGame {
            id: GameId::Executable(exe_path.to_path_buf()),
            display_name,
            install_path: exe_path.to_path_buf(),
            icon_path: None,
            source: Source::Manual {
                root: root.to_path_buf(),
            },
            enabled: false,
        });
    });
    games
}

fn walk_exes(dir: &Path, max_depth: usize, cb: &mut impl FnMut(&Path)) {
    if max_depth == 0 {
        return;
    }
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            walk_exes(&path, max_depth - 1, cb);
        } else if path
            .extension()
            .and_then(|s| s.to_str())
            .map(|s| s.eq_ignore_ascii_case("exe"))
            .unwrap_or(false)
        {
            cb(&path);
        }
    }
}

// ============================================================================
// Persistence
// ============================================================================

fn state_path() -> PathBuf {
    crate::profiles::config_dir().join("games.json")
}

pub fn load() -> Option<GamesState> {
    let body = fs::read_to_string(state_path()).ok()?;
    serde_json::from_str(&body).ok()
}

pub fn save(state: &GamesState) -> std::io::Result<()> {
    let path = state_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let body = serde_json::to_string_pretty(state).map_err(|e| {
        std::io::Error::new(std::io::ErrorKind::InvalidData, e)
    })?;
    fs::write(path, body)
}

/// Merge a fresh scan into an existing state: preserve the `enabled`
/// flag from `old` when the same `GameId` reappears, drop games no
/// longer found. Does NOT touch `scan_locations`.
pub fn merge_scan(old: &GamesState, scanned: Vec<DetectedGame>) -> Vec<DetectedGame> {
    let mut enabled_lookup: HashMap<GameId, bool> = HashMap::new();
    for g in &old.games {
        enabled_lookup.insert(g.id.clone(), g.enabled);
    }
    scanned
        .into_iter()
        .map(|mut g| {
            if let Some(was) = enabled_lookup.get(&g.id) {
                g.enabled = *was;
            }
            g
        })
        .collect()
}

/// First-run / cold-start helper: load state from disk if present,
/// otherwise seed with hardcoded defaults + an immediate scan.
///
/// On every launch (not just first-run) we re-run `scan_all` against
/// the stored `scan_locations` and merge the result — this picks up
/// newly-installed games, drops ones that have been uninstalled or
/// that now fail the tool filter, and refreshes icon paths as Steam's
/// art cache fills in over time. Enabled toggles are preserved across
/// the rescan by matching on `GameId`.
pub fn load_or_init() -> GamesState {
    if let Some(existing) = load() {
        let scanned = scan_all(&existing.scan_locations);
        let merged = merge_scan(&existing, scanned);
        let mut state = existing;
        state.games = merged;
        if let Err(e) = save(&state) {
            log::warn!("failed to persist refreshed games.json: {e}");
        }
        return state;
    }
    let scan_locations = default_scan_locations();
    let games = scan_all(&scan_locations);
    let state = GamesState {
        scan_locations,
        games,
    };
    if let Err(e) = save(&state) {
        log::warn!("failed to seed games.json: {e}");
    }
    state
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIBRARYFOLDERS_SAMPLE: &str = r#"
"libraryfolders"
{
	"0"
	{
		"path"		"/home/user/.local/share/Steam"
		"label"		""
		"apps"
		{
			"228980"		"142521834"
			"1493710"		"1449166491"
		}
	}
}
"#;

    const APPMANIFEST_SAMPLE: &str = r#"
"AppState"
{
	"appid"		"1493710"
	"name"		"Proton Experimental"
	"installdir"		"Proton - Experimental"
}
"#;

    #[test]
    fn parses_libraryfolders() {
        let parsed = parse_vdf(LIBRARYFOLDERS_SAMPLE).expect("parse");
        let root = parsed.get("libraryfolders").unwrap().as_block().unwrap();
        let zero = root.get("0").unwrap().as_block().unwrap();
        assert_eq!(zero.get("path").unwrap().as_str().unwrap(), "/home/user/.local/share/Steam");
    }

    #[test]
    fn parses_appmanifest() {
        let parsed = parse_vdf(APPMANIFEST_SAMPLE).expect("parse");
        let state = parsed.get("AppState").unwrap().as_block().unwrap();
        assert_eq!(state.get("appid").unwrap().as_str().unwrap(), "1493710");
        assert_eq!(state.get("name").unwrap().as_str().unwrap(), "Proton Experimental");
    }
}
