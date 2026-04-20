#ifndef GFF_OVERLAY_THEME_HPP_INCLUDED
#define GFF_OVERLAY_THEME_HPP_INCLUDED

struct ImFont;

// Project-specific ImGui theme: translucent, hairline borders, cool-cyan
// accents. Applied once during overlay setup in place of the default
// vkBasalt_overlay styling.

namespace gff
{
    // Load project fonts into the current ImGui context's atlas. Must be
    // called AFTER ImGui::CreateContext() and BEFORE ImGui_ImplVulkan_Init()
    // (the latter uploads the atlas to the GPU). Safe to call multiple times
    // but the second call is a no-op.
    void loadFonts();

    // Returns the regular / bold fonts, or nullptr if loading failed and
    // we're using ImGui's built-in ProggyClean. Callers should null-check.
    ImFont* regularFont();
    ImFont* boldFont();

    // Apply our theme to the current ImGui context. Idempotent: can be called
    // more than once (e.g. after a font atlas rebuild) without side effects.
    void applyOverlayTheme();
}

#endif // GFF_OVERLAY_THEME_HPP_INCLUDED
