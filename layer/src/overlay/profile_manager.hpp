#ifndef GFF_PROFILE_MANAGER_HPP_INCLUDED
#define GFF_PROFILE_MANAGER_HPP_INCLUDED

#include <string>

namespace vkBasalt
{
    class EffectRegistry;
    class ImGuiOverlay;
}

// Per-game profile management for the GameFiltersFlatpak overlay.
//
// Each game gets three profile slots stored at
//     $XDG_CONFIG_HOME/game-filters-flatpak/games/<exe>/profile{1,2,3}.conf
// plus an `active.txt` marker recording which slot the user last used.
// On overlay init we pick up where the user left off; on slider change we
// persist the current state to the active slot automatically.
namespace gff
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
}

#endif // GFF_PROFILE_MANAGER_HPP_INCLUDED
