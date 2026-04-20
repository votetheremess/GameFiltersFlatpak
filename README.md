# GameFiltersFlatpak

A Linux Flatpak that replicates Nvidia's Freestyle / Game Filters experience. Install once, press a hotkey in any Vulkan game, tweak color / contrast / sharpening sliders with live in-game preview. System-tray daemon keeps the controller in the background.

**Status:** scaffolded. Repo structure, Vulkan layer fork, implicit-layer manifest, Rust frontend skeleton, IPC protocol, and a combined V1 filter shader are all in place. End-to-end build + run is still TODO — the layer and shader code have not yet been compiled against the real Vulkan SDK on the target machine.

## What's different from existing Linux tools

No per-game launch options. vkBasalt and vkPost require `ENABLE_VKBASALT=1 %command%` in every game's Steam launch options; we install as an **implicit** Vulkan layer so the Vulkan loader auto-loads us for every Vulkan application. That's the single biggest UX difference.

No multi-tab admin UI in-game. Our overlay is a single-column slider panel sized for the way the user actually interacts with these filters (drag, look, dismiss). Power-user views from upstream are deferred.

## Architecture

Two Flatpak artifacts, one socket:

- **`com.gamefiltersflatpak.VulkanLayer`** — Vulkan post-processing layer (C++, forked from [vkBasalt_overlay](https://github.com/Boux/vkBasalt_overlay), rebranded and converted to implicit). Lives at `layer/`.
- **`com.gamefiltersflatpak.App`** — GTK4 + libadwaita frontend (Rust, matches the user's SteelSeries Flatpak stack). Tray icon, XDG GlobalShortcuts portal client, per-game profile management, IPC client to the layer. Lives at `frontend/`.

They communicate over `$XDG_RUNTIME_DIR/game-filters-flatpak.sock` using length-prefixed JSON — see [`docs/ipc-protocol.md`](docs/ipc-protocol.md).

## V1 Filter Scope

Combined single-pass shader at `layer/src/shader/gff.frag.glsl` with eight parameters:

- Brightness, Contrast (with Highlights, Shadows split)
- Temperature, Vibrance (perceptual, not naive HSL)
- Gamma
- HDR Toning (classical tonemap approximation)

Plus the existing CAS pass for Sharpen. Later versions will add Tilt-Shift, Painterly, DoF, Splitscreen, Bloom, Black & White, Vignette, Depth of Field, and RTX HDR / RTX Dynamic Vibrance approximations.

## Supported Environments (v1)

- Bazzite KDE (Plasma Wayland)
- Any Vulkan game, including DirectX titles running through Proton via DXVK / VKD3D-Proton

Explicitly **not** supported in v1: native OpenGL games, GNOME, Steam Deck gaming-mode (Gamescope), X11 desktop sessions. The Flatpak manifest retains `--socket=fallback-x11` only so GTK itself starts cleanly under unusual setups — it does not enable an X11 input path in v1.

## Building

Host is Bazzite (immutable Fedora Atomic) — no dev headers on the host. Edit on host, build in the `fedora-dev` distrobox, run on host.

```sh
# Layer (C++, Meson)
distrobox enter fedora-dev -- meson setup layer/builddir layer
distrobox enter fedora-dev -- meson compile -C layer/builddir

# Frontend (Rust, Cargo)
distrobox enter fedora-dev -- cargo build --manifest-path frontend/Cargo.toml

# Run on host
./frontend/target/debug/game-filters-flatpak
```

Flatpak build (once ready for release):

```sh
distrobox enter fedora-dev -- flatpak-builder --user --install --force-clean \
  build-flatpak flatpak/com.gamefiltersflatpak.VulkanLayer.yml
distrobox enter fedora-dev -- flatpak-builder --user --install --force-clean \
  build-flatpak flatpak/com.gamefiltersflatpak.App.yml
```

Verify (on host):

```sh
vulkaninfo | grep -i game-filters    # layer auto-loaded
vkcube                                 # pass-through smoke test
```

## Package requirements (inside `fedora-dev` distrobox)

```sh
sudo dnf install -y \
  meson ninja-build \
  vulkan-headers vulkan-loader-devel \
  glslang spirv-headers spirv-tools \
  libX11-devel libXi-devel \
  gtk4-devel libadwaita-devel \
  flatpak-builder
```

(The GTK4 / libadwaita dev packages are likely already there from your SteelSeries work.)

## Repo layout

- `layer/` — C++ Vulkan layer (Meson). Forked from vkBasalt_overlay; upstream Zlib notice at `layer/LICENSE-UPSTREAM`.
- `frontend/` — Rust GTK4 + libadwaita app (Cargo).
- `flatpak/` — Flatpak manifests for both artifacts.
- `docs/` — architecture + IPC protocol reference.

## License

MIT for code authored here (frontend, shader, IPC, theme, manifests). Zlib for the Vulkan-layer renderer inherited from vkBasalt / vkBasalt_overlay — see `layer/LICENSE-UPSTREAM`.
