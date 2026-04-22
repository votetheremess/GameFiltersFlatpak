# Architecture

Two separately-packaged Flatpak artifacts that communicate over Unix sockets. The Vulkan layer runs inside every Vulkan app on the system; the frontend runs once, as a user-session daemon with a tray icon.

## Vulkan Layer — `org.freedesktop.Platform.VulkanLayer.Lumen`

C++, Meson. Forked from [vkBasalt_overlay](https://github.com/Boux/vkBasalt_overlay), which itself descends from vkBasalt. We inherit the Vulkan hooking, ImGui integration, effect registry, shader pipeline, and ReShade FX compiler. We own the visual design, theme, layered sidebar UI, branding, implicit-layer loader model, per-game profile system, IPC client, abstract-socket support, font loader, and the four chained Lumen effects (`lumen_local`, `lumen_tonal`, `lumen_color`, `lumen_stylistic`). Upstream Zlib notice preserved at `layer/LICENSE-UPSTREAM`.

Installed as an **implicit** Vulkan layer — the Vulkan loader auto-loads it into every Vulkan application on the system, the same mechanism `VK_LAYER_MESA_device_select` uses. No per-game `ENABLE_VK*=1` launch options required. This is the single most important UX differentiator vs existing Linux post-processing tools (vkBasalt, vkPost, upstream vkBasalt_overlay all ship as explicit layers).

Being implicit means we're loaded into processes that are not games: KWin, GTK/Qt apps, Wine helpers. Two gates decide whether a given process gets the full layer or a zero-cost pass-through:

1. **`lumen::isGameProcess()`** — true iff `argv[0]` ends in `.exe`/`.EXE`, or one of `SteamAppId` / `SteamGameId` / `STEAM_COMPAT_DATA_PATH` is in the environment. Cached per-process.
2. **`lumen::frontendAvailable()`** — probes the IPC socket (filesystem path + abstract namespace); result cached 500 ms so swapchain-create bursts don't storm the frontend.

Both gates apply at `vkBasalt_CreateSwapchainKHR`. When either is false, the swapchain is marked `bypassLayer = true` and every hook (`GetSwapchainImagesKHR`, `QueuePresentKHR`, `DestroySwapchainKHR`) checks the flag at the top and forwards to the driver untouched. The persistent IPC connection is also gated on `isGameProcess()`, so non-game processes never open a socket.

### Wine-safe VkResult

Every `VkResult` that `vkBasalt_QueuePresentKHR` returns to a Wine/Proton caller must be spec-legal — Wine's winevulkan thunk asserts on anything outside {`VK_SUCCESS`, `VK_SUBOPTIMAL_KHR`, `VK_ERROR_OUT_OF_DATE_KHR`, `VK_ERROR_SURFACE_LOST_KHR`, `VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT`, `VK_ERROR_OUT_OF_{HOST,DEVICE}_MEMORY`, `VK_ERROR_DEVICE_LOST`}. Our internal submit helpers can fail with other codes; when they do, we fall back to the driver's native `vkQueuePresentKHR` on the original un-wrapped present info — the user loses effect application for that frame, not the game.

### Overlay and input blocking

In-game UI is a left-edge full-height sidebar, position-locked (`ImGuiWindowFlags_NoMove`, pinned to `viewport->WorkPos`). A three-slot profile selector at the top, then a Freestyle-style filter stack: users start with no filters active, add cards with `Add`, reorder with `Up`/`Down`, and remove with `Remove`. Four canonical cards map to the four Lumen effects (Brightness/Contrast → `lumen_tonal`, Color → `lumen_color`, Details → `lumen_local`, Effects → `lumen_stylistic`). Sliders are live — changes flow through the registry into spec-constant rebakes on the next frame. When the overlay is open, XInput2 `XIGrabDevice` grabs every master and attached-slave pointer/keyboard so the game doesn't see mouse motion or keystrokes. `XIGrabDevice` covers both core X events and `XI_RawMotion`, which Wine/DXVK titles read for mouse input and which the legacy `XGrabPointer` doesn't catch.

### Filter pipeline

Four chained fragment-shader passes, each its own `SimpleEffect` subclass with its own SPIR-V pipeline and specialization constants. Upstream vkBasalt's chain infrastructure (`LogicalSwapchain::effects` vector, iterated in `command_buffer.cpp`) runs each effect as a separate render pass, feeding pass N's output to pass N+1's input. Chain order comes from the registry's `selectedEffects` list, persisted in each profile's `effects = a:b:c` line. Because each pass reads the previous pass's output (not the original swapchain image), users can freely reorder cards without the "spatial filter sees already-graded neighbors" problem that a single-pass design would have.

| Effect | Shader | Sliders |
|---|---|---|
| `lumen_local` | `lumen_local.frag.glsl` | Sharpen, Clarity, HDR Toning, Bloom |
| `lumen_tonal` | `lumen_tonal.frag.glsl` | Exposure, Contrast, Highlights, Shadows, Gamma |
| `lumen_color` | `lumen_color.frag.glsl` | Tint Color, Tint Intensity, Temperature, Vibrance |
| `lumen_stylistic` | `lumen_stylistic.frag.glsl` | Vignette, Black & White |

Sliders follow Nvidia's public scale (bipolar ±100, unipolar 0–100, hue 0–360) so Windows preset values paste in directly. Each shader normalizes internally. Every Lumen effect pins its image views to the UNORM alias of the swapchain format via `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, so the hardware sampler neither auto-linearizes on read nor re-encodes on write. Rec.601 luma matches Freestyle's reference against gamma-encoded signal.

Intermediate render targets between passes are 8-bit UNORM (matching Freestyle). If gradient content ever reveals cumulative quantization banding, the `SimpleEffect::init(..., viewFormat)` override already supports bumping intermediates to 16-bit at the cost of extra VRAM.

Tonal math (Exposure / Contrast / Shadows / Highlights / Gamma) stays bundled in one shader — the stages depend on each other's intermediate values and splitting them across passes reintroduces the clamp-then-shape problem the combined form avoids. Card-level split only.

Sign conventions (empirical, from YouTube Freestyle reference):

- `+shadows` → DARKER darks, `-shadows` → BRIGHTER darks (Nvidia convention, opposite of Lightroom "lift")
- `+highlights` → RECOVERY / darken brights (Lightroom convention)
- `+gamma` → brighter midtones (Nvidia and Lightroom agree)

See the four `lumen_*.frag.glsl` shaders for full parameter lists and math.

## Frontend — `io.github.votetheremess.Lumen`

Rust + gtk4-rs + libadwaita-rs. Tray via `ksni` (StatusNotifierItem). Global hotkey via the XDG GlobalShortcuts portal (`ashpd`). Per-game profile management. IPC **server**.

The frontend sets `LUMEN_DISABLE=1` in its own process environment at the top of `main.rs`, *before* any GTK/Adw initialization — otherwise GTK4's internal Vulkan rendering loads our layer inside the frontend process, and that in-process instance wins socket contention, swallowing every command meant for the game.

Window close semantics:

- **Tray → Quit** — clean exit; `SocketGuard::drop` unlinks the filesystem socket.
- **Ctrl+C in `scripts/run.sh` terminal** — EXIT trap kills the frontend and removes the socket.
- **Window X button** — hides to tray, does not quit (matches the Nvidia Freestyle daemon-in-background model).

## IPC

**Frontend is the server; every layer instance is a client.** Originally designed the other way round; flipped because multiple Vulkan processes on the system were fighting for socket ownership. The frontend broadcasts commands (overlay toggle, profile switch) to every connected layer.

Listens on two sockets simultaneously:

- Filesystem: `$XDG_RUNTIME_DIR/lumen.sock`
- Abstract: `@lumen.sock`

The abstract socket is how layers inside Steam's pressure-vessel sandbox reach the frontend — the filesystem path is isolated in that namespace.

Framing is length-prefixed little-endian JSON, capped at 64 KiB per message. See [`docs/ipc-protocol.md`](ipc-protocol.md) for message schema and reference.

### Frontend-availability gating

Closing the frontend disables filters. The layer's persistent `IpcClient` fires a `ConnectionHandler` on every connect/disconnect edge; the present thread consumes the event and calls `lumen::applyActiveProfile()` or `lumen::applyNeutral()` accordingly. Both mark the overlay dirty, so spec constants rebake on the next frame. Launching a game with the frontend closed yields neutral pass-through; relaunching the frontend mid-game re-applies the active profile within a frame.

## Per-game profiles

State under `~/.config/lumen/games/<exe>/`:

- `profile1.conf`, `profile2.conf`, `profile3.conf` — three slots, auto-created on first launch
- `active.txt` — integer 1-3 identifying the selected slot

`<exe>` is the basename of `/proc/self/cmdline` with `.exe`/`.EXE` stripped (Proton games typically report `Cyberpunk2077`, `PioneerGame`, etc.). Falls through to `/proc/self/comm`, then literal `default`.

Every slider change writes synchronously (no debounce needed — files are tiny) and calls `overlay->markDirty()`. Switching profiles is an atomic value swap plus another `markDirty()`; forgetting the `markDirty()` is the single easiest bug — the UI numbers update but the pipeline keeps rendering old values because spec constants are baked into the old pipeline.

## V1 scope (locked)

- Target DE: Bazzite KDE Plasma Wayland only. GNOME, Steam Deck Gamescope, X11 desktop sessions deferred.
- APIs: Vulkan + DXVK/VKD3D only. Native OpenGL deferred.
- Filters: 15 classical Freestyle parameters. RTX HDR / RTX Dynamic Vibrance filters deferred (need ONNX / Tensor-Core work).
