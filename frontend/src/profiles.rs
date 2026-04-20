use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct Profile {
    pub name: String,
    #[serde(default)]
    pub brightness: f32,
    #[serde(default = "one")]
    pub contrast: f32,
    #[serde(default)]
    pub highlights: f32,
    #[serde(default)]
    pub shadows: f32,
    #[serde(default)]
    pub temperature: f32,
    #[serde(default)]
    pub vibrance: f32,
    #[serde(default)]
    pub sharpen: f32,
    #[serde(default = "one")]
    pub gamma: f32,
    #[serde(default)]
    pub hdr_toning: f32,
}

fn one() -> f32 {
    1.0
}

pub fn config_dir() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            std::env::var_os("HOME")
                .map(|h| PathBuf::from(h).join(".config"))
                .unwrap_or_else(|| PathBuf::from("/tmp"))
        });
    base.join("game-filters-flatpak")
}

pub fn profile_path(name: &str) -> PathBuf {
    config_dir().join("profiles").join(format!("{name}.conf"))
}

pub fn active_config_path() -> PathBuf {
    config_dir().join("active.conf")
}

pub fn list_profiles() -> Vec<String> {
    let dir = config_dir().join("profiles");
    let Ok(entries) = std::fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut names: Vec<String> = entries
        .flatten()
        .filter_map(|e| {
            let path = e.path();
            if path.extension()?.to_str()? == "conf" {
                path.file_stem().and_then(|s| s.to_str()).map(str::to_owned)
            } else {
                None
            }
        })
        .collect();
    names.sort();
    names
}

/// Write a profile to disk as the active profile the layer reads.
///
/// The config file format is the `key = value` style the layer inherited
/// from vkBasalt_overlay; see `layer/src/config.cpp`.
pub fn write_active(profile: &Profile) -> std::io::Result<()> {
    let path = active_config_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let body = format_conf(profile);
    std::fs::write(path, body)
}

fn format_conf(p: &Profile) -> String {
    format!(
        "# Written by game-filters-flatpak frontend\n\
         effects = gff_pipeline\n\
         gff.brightness = {bright}\n\
         gff.contrast = {contrast}\n\
         gff.highlights = {hi}\n\
         gff.shadows = {lo}\n\
         gff.temperature = {temp}\n\
         gff.vibrance = {vib}\n\
         gff.sharpen = {sharp}\n\
         gff.gamma = {gamma}\n\
         gff.hdrToning = {hdr}\n",
        bright = p.brightness,
        contrast = p.contrast,
        hi = p.highlights,
        lo = p.shadows,
        temp = p.temperature,
        vib = p.vibrance,
        sharp = p.sharpen,
        gamma = p.gamma,
        hdr = p.hdr_toning,
    )
}

pub fn load(name: &str) -> std::io::Result<Profile> {
    let path = profile_path(name);
    let body = std::fs::read_to_string(path)?;
    serde_json::from_str(&body).map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))
}

pub fn save(profile: &Profile) -> std::io::Result<()> {
    let path = profile_path(&profile.name);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let body = serde_json::to_string_pretty(profile)?;
    std::fs::write(path, body)
}

pub fn delete(name: &str) -> std::io::Result<()> {
    let path = profile_path(name);
    if path.exists() {
        std::fs::remove_file(path)?;
    }
    Ok(())
}

pub fn exe_basename(exe: &str) -> String {
    Path::new(exe)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or(exe)
        .to_owned()
}
