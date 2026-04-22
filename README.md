# Lumen

A Linux Flatpak that replicates Nvidia's Freestyle / Game Filters experience. Install once, press a hotkey in any Vulkan game, tweak color / contrast / sharpening sliders with live in-game preview. System-tray daemon keeps the controller in the background.

**Status:** pre-release. End-to-end build and in-game overlay work on Bazzite KDE Plasma Wayland; profiles persist per-game; Freestyle-style add/remove/reorder of filter cards lands against Arc Raiders and other Vulkan titles via Flatpak Steam. Flatpak packaging is ready to submit to Flathub (pending icon and screenshot assets).

## What's different from existing Linux tools

No per-game launch options. vkBasalt and vkPost require `ENABLE_VKBASALT=1 %command%` in every game's Steam launch options; we install as an **implicit** Vulkan layer so the Vulkan loader auto-loads us for every Vulkan application. That's the single biggest UX difference.

No multi-tab admin UI in-game. Our overlay is a single-column slider panel sized for the way the user actually interacts with these filters (drag, look, dismiss). Power-user views from upstream are deferred.

## Architecture

Two Flatpak artifacts, one socket:

- **`org.freedesktop.Platform.VulkanLayer.Lumen`** — Vulkan post-processing layer (C++, forked from [vkBasalt_overlay](https://github.com/Boux/vkBasalt_overlay), rebranded and converted to implicit). Shipped as a Flathub Vulkan-layer extension so Flatpak Steam auto-loads it into Proton games. Lives at `layer/`.
- **`io.github.votetheremess.Lumen`** — GTK4 + libadwaita frontend (Rust). Tray icon, XDG GlobalShortcuts portal client, per-game profile management, IPC server to the layer. Lives at `frontend/`.

They communicate over `$XDG_RUNTIME_DIR/lumen.sock` using length-prefixed JSON — see [`docs/ipc-protocol.md`](docs/ipc-protocol.md).

## V1 Filter Scope

Four chained fragment-shader passes, each with its own sliders. The overlay starts empty — press **Add** on a card to activate it, **Up** / **Down** to reorder, **Remove** to drop it. Shaders live at `layer/src/shader/lumen_{local,tonal,color,stylistic}.frag.glsl`.

| Card | Sliders |
|---|---|
| **Brightness / Contrast** | Exposure, Contrast, Highlights, Shadows, Gamma |
| **Color** | Tint Color (hue), Tint Intensity, Temperature, Vibrance |
| **Details** | Sharpen, Clarity, HDR Toning, Bloom |
| **Effects** | Vignette, Black & White |

Slider ranges follow Nvidia's public scale (bipolar ±100, unipolar 0–100, hue 0–360) so Windows preset values paste in directly. Upstream's CAS pass is also available — chain it ahead of the Lumen cards for higher-quality adaptive sharpening. Tilt-Shift, Painterly, Depth of Field, and RTX HDR / RTX Dynamic Vibrance approximations are deferred post-v1.

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
./frontend/target/debug/lumen
```

Flatpak build (once ready for release):

```sh
distrobox enter fedora-dev -- flatpak-builder --user --install --force-clean \
  build-layer flatpak/org.freedesktop.Platform.VulkanLayer.Lumen.yml
distrobox enter fedora-dev -- flatpak-builder --user --install --force-clean \
  build-app flatpak/io.github.votetheremess.Lumen.yml
```

Verify (on host):

```sh
vulkaninfo | grep -i lumen    # layer auto-loaded
vkcube                          # pass-through smoke test
flatpak run io.github.votetheremess.Lumen    # launches the tray daemon + GTK window
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
