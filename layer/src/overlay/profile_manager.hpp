#ifndef LUMEN_PROFILE_MANAGER_HPP_INCLUDED
#define LUMEN_PROFILE_MANAGER_HPP_INCLUDED

#include <string>
#include <vector>

namespace vkBasalt
{
    class EffectRegistry;
    class ImGuiOverlay;
}

// Per-game profile management for the Lumen overlay.
//
// Each game gets three profile slots stored at
//     $XDG_CONFIG_HOME/lumen/games/<exe>/profile{1,2,3}.conf
// plus an `active.txt` marker recording which slot the user last used.
// On overlay init we pick up where the user left off; on slider change we
// persist the current state to the active slot automatically.
namespace lumen
{
    struct ProfileState
    {
        std::string gameName;       // basename of /proc/self/cmdline (.exe stripped)
        int         activeProfile = 1;  // 1, 2, or 3
    };

    // Read the currently-detected game name and active slot. Returns a
    // reference to the process-wide singleton; safe to read from the
    // overlay render thread.
    const ProfileState& state();

    // Bootstrap: detect game, create dir + 3 empty profile files on first
    // run, load the last-used profile's values into the registry / config.
    // If the frontend is not reachable the layer goes neutral (all zeros)
    // so a closed frontend means filters off, matching user expectation.
    // Idempotent — multiple calls are no-ops after the first.
    void initializeForGame(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay);

    // Activate profile N (1-3). Loads that profile's values into the
    // registry and config-overrides, updates active.txt, marks the overlay
    // dirty so the next frame rebuilds the effect with new spec constants.
    void switchProfile(int n, vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay);

    // Persist the current registry values to the active profile file.
    // Called on every slider change; writes are small (<1 KiB) and
    // synchronous — cheap enough that we don't bother debouncing.
    void saveActiveProfile(vkBasalt::EffectRegistry* reg);

    // Re-apply the currently-active profile from disk. Called by the IPC
    // layer when the frontend reconnects after being absent.
    void applyActiveProfile(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay);

    // Apply all-zero values (pass-through). Called by the IPC layer when
    // the frontend connection drops or fails to establish.
    void applyNeutral(vkBasalt::EffectRegistry* reg, vkBasalt::ImGuiOverlay* overlay);

    // --- Filter stack (add / remove / reorder) --------------------------
    //
    // The overlay exposes four cards — Brightness/Contrast, Color, Details,
    // Effects — each backed by one effect in the chain (lumen_tonal, lumen_color,
    // lumen_local, lumen_stylistic). Users activate cards with "+", remove with
    // "−", and reorder with "↑ ↓". State lives in the registry's selected-
    // effects list and is persisted to the active profile's `effects = ...`
    // line.

    struct CardSlider
    {
        const char* key;    // Parameter key, e.g. "lumen.exposure"
        const char* label;  // Human-readable slider label
        const char* fmt;    // ImGui format string (e.g. "%.0f" or "%.0f\xc2\xb0")
    };

    struct CardDef
    {
        const char*             effectName;  // Built-in effect name, e.g. "lumen_tonal"
        const char*             title;       // Section header, e.g. "Brightness / Contrast"
        std::vector<CardSlider> sliders;     // Sliders owned by this card, in display order
    };

    // Canonical card list. Order here is the migration/default order used
    // when legacy profiles are upgraded and inactive cards are listed in
    // the "Add filter" section.
    const std::vector<CardDef>& cards();

    // Append `effectName` to the end of the active chain if not already
    // present. Persists to disk and triggers a debounced swapchain reload.
    void addCard(const std::string&       effectName,
                 vkBasalt::EffectRegistry* reg,
                 vkBasalt::ImGuiOverlay*   overlay);

    // Remove `effectName` from the active chain. Slider values for the
    // card are preserved on disk so re-adding later restores them.
    void removeCard(const std::string&       effectName,
                    vkBasalt::EffectRegistry* reg,
                    vkBasalt::ImGuiOverlay*   overlay);

    // Swap `effectName` with its neighbor in the chain (delta = -1 for up,
    // +1 for down). No-op if the move would fall off either end.
    void moveCard(const std::string&       effectName,
                  int                       delta,
                  vkBasalt::EffectRegistry* reg,
                  vkBasalt::ImGuiOverlay*   overlay);

    // Best-effort probe: returns true if the frontend's IPC socket is
    // reachable right now (filesystem path or abstract namespace).
    bool frontendAvailable();

    // Heuristic: does this process look like a game we should apply
    // filters to? Our layer is implicit so it auto-loads into *every*
    // Vulkan-using process on the system — native GTK/Qt apps, system
    // utilities, the KDE compositor. Without this gate they'd all get
    // toggle-overlay broadcasts and try to render an ImGui sidebar on
    // top of themselves. The check passes for:
    //   - Wine/Proton processes (argv[0] ends in .exe)
    //   - Steam-launched processes (SteamAppId / SteamGameId set)
    //   - Proton-launched processes (STEAM_COMPAT_DATA_PATH set)
    // and fails for everything else. Cached on first call — the answer
    // doesn't change within a process lifetime.
    bool isGameProcess();

    // Second-stage gate: is THIS specific game one the user has toggled
    // ON in the frontend window? Checked at vkCreateSwapchainKHR after
    // isGameProcess() passes. Matches the process's SteamAppId env var
    // against the enabled Steam app-id set, OR its argv[0] basename
    // against the enabled-exe basename set (populated from the scanner's
    // manual-dir detections). Result is NOT cached — the enabled set
    // mutates at runtime as `games-enabled-update` IPC messages arrive.
    bool isGameEnabled();

    // Parse a "games-enabled-update" IPC payload and replace the cached
    // enabled-games sets atomically. Called from the IPC handler in
    // basalt.cpp. Minimal hand-rolled JSON extraction (no JSON lib in
    // the layer) — tolerates the exact shape the Rust frontend emits,
    // doesn't attempt full JSON validation.
    void applyEnabledGamesJson(const std::string& rawJson);
}

#endif // LUMEN_PROFILE_MANAGER_HPP_INCLUDED
