#include "theme_lumen.hpp"

#include <array>
#include <sys/stat.h>

#include "imgui/imgui.h"
#include "logger.hpp"

namespace lumen
{
    namespace
    {
        ImFont* g_regular = nullptr;
        ImFont* g_bold    = nullptr;
        bool    g_loaded  = false;

        // Font size at 1x scale. We avoid FontGlobalScale bumps — scaling
        // bitmap font atlases blurs them — instead we pre-render at 16px.
        constexpr float kFontSize = 16.0f;

        // Candidate system font paths, in preference order. First existing
        // match wins. We pair regular+bold from the same family so bold
        // headers feel like the same face at heavier weight.
        //
        // Each entry is checked under TWO root prefixes — the host root `/`
        // (works for native apps and for the frontend) AND the pressure-vessel
        // sandbox root `/run/host/` (where Steam Proton games see the host
        // filesystem mounted, since their own root is the Steam Runtime).
        struct FontPair
        {
            const char* regular;
            const char* bold;
        };

        constexpr std::array<FontPair, 7> kFontCandidates = {{
            // Inter — install via `dnf install rsms-inter-fonts`. Crispest.
            {"share/fonts/inter/Inter-Regular.ttf", "share/fonts/inter/Inter-Bold.ttf"},
            // Adwaita Sans — GNOME's new default in Fedora 43+.
            {"share/fonts/adwaita-sans-fonts/AdwaitaSans-Regular.ttf",
             "share/fonts/adwaita-sans-fonts/AdwaitaSans-Regular.ttf"},
            // Cantarell — long-time GNOME default, ships with abattis pkg.
            {"share/fonts/abattis-cantarell-fonts/Cantarell-Regular.otf",
             "share/fonts/abattis-cantarell-fonts/Cantarell-Bold.otf"},
            // Noto Sans — google-noto pkg, ubiquitous.
            {"share/fonts/google-noto/NotoSans-Regular.ttf",
             "share/fonts/google-noto/NotoSans-Bold.ttf"},
            // Liberation Sans — fedora-core, reliable.
            {"share/fonts/liberation-sans/LiberationSans-Regular.ttf",
             "share/fonts/liberation-sans/LiberationSans-Bold.ttf"},
            // DejaVu Sans — near-universal last resort.
            {"share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
             "share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"},
            // Droid Sans — fallback for older / minimal installs.
            {"share/fonts/google-droid-sans-fonts/DroidSans.ttf",
             "share/fonts/google-droid-sans-fonts/DroidSans-Bold.ttf"},
        }};

        // Root prefixes searched for each font path, in order.
        constexpr std::array<const char*, 3> kRootPrefixes = {{
            "/usr/",          // Native apps and Flatpak host extension.
            "/run/host/usr/", // Steam pressure-vessel sandbox — host fs mounted here.
            "/app/",          // Flatpak app sandbox extension prefix.
        }};

        bool fileExists(const std::string& path)
        {
            struct stat st{};
            return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        }

        // Returns the first existing absolute path constructed from any
        // (prefix, relative) pair, or empty string if none exist.
        std::string firstExistingPath(const char* relative)
        {
            if (!relative)
                return {};
            for (const char* prefix : kRootPrefixes)
            {
                std::string full = std::string(prefix) + relative;
                if (fileExists(full))
                    return full;
            }
            return {};
        }
    } // namespace

    void loadFonts()
    {
        if (g_loaded)
            return;
        g_loaded = true;

        ImGuiIO& io = ImGui::GetIO();

        for (const auto& pair : kFontCandidates)
        {
            std::string regularPath = firstExistingPath(pair.regular);
            if (regularPath.empty())
                continue;
            ImFont* regular = io.Fonts->AddFontFromFileTTF(regularPath.c_str(), kFontSize);
            if (!regular)
                continue;
            g_regular = regular;

            std::string boldPath = firstExistingPath(pair.bold);
            if (!boldPath.empty() && boldPath != regularPath)
            {
                ImFont* bold = io.Fonts->AddFontFromFileTTF(boldPath.c_str(), kFontSize);
                if (bold)
                    g_bold = bold;
            }
            io.FontDefault = g_regular;
            vkBasalt::Logger::info(std::string("theme: loaded font ") + regularPath
                                   + (g_bold ? std::string(" + bold ") + boldPath : " (no bold variant)"));
            return;
        }
        vkBasalt::Logger::warn("theme: no system fonts found in /usr/share/fonts or /run/host/usr/share/fonts;"
                               " falling back to ImGui ProggyClean");
    }

    ImFont* regularFont() { return g_regular; }
    ImFont* boldFont()    { return g_bold; }

    void applyOverlayTheme()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Layout — tighter than ImGui defaults, room to breathe, matches the
        // SteelSeries-but-lighter feel (less "admin panel", more "consumer").
        style.WindowRounding     = 12.0f;
        style.ChildRounding      = 10.0f;
        style.FrameRounding      = 6.0f;
        style.PopupRounding      = 10.0f;
        style.ScrollbarRounding  = 8.0f;
        style.GrabRounding       = 6.0f;
        style.TabRounding        = 6.0f;

        style.WindowPadding      = ImVec2(18.0f, 18.0f);
        style.FramePadding       = ImVec2(10.0f, 7.0f);
        style.ItemSpacing        = ImVec2(10.0f, 9.0f);
        style.ItemInnerSpacing   = ImVec2(8.0f, 6.0f);
        style.IndentSpacing      = 18.0f;
        style.ScrollbarSize      = 12.0f;
        style.GrabMinSize        = 14.0f;

        style.WindowBorderSize   = 1.0f;
        style.ChildBorderSize    = 1.0f;
        style.PopupBorderSize    = 1.0f;
        style.FrameBorderSize    = 0.0f;
        style.TabBorderSize      = 0.0f;

        style.WindowTitleAlign   = ImVec2(0.02f, 0.5f);
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
        style.Alpha              = 1.0f;

        // Palette. 70% window opacity lets the game remain visible; a
        // single-pixel white-at-10% border is the hairline containment line.
        const ImVec4 bg_window    = ImVec4(0.08f, 0.08f, 0.10f, 0.70f);
        const ImVec4 bg_child     = ImVec4(0.08f, 0.08f, 0.10f, 0.55f);
        const ImVec4 bg_popup     = ImVec4(0.08f, 0.08f, 0.10f, 0.92f);
        const ImVec4 bg_frame     = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
        const ImVec4 bg_frame_hov = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
        const ImVec4 bg_frame_act = ImVec4(1.00f, 1.00f, 1.00f, 0.14f);
        const ImVec4 border       = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
        const ImVec4 border_shadow = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        const ImVec4 text         = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
        const ImVec4 text_dim     = ImVec4(0.60f, 0.63f, 0.70f, 1.00f);
        const ImVec4 accent       = ImVec4(0.00f, 0.96f, 1.00f, 0.85f);   // #00F5FF electric cyan
        const ImVec4 accent_hov   = ImVec4(0.30f, 1.00f, 1.00f, 0.95f);   // brighter hover variant
        const ImVec4 accent_act   = ImVec4(0.60f, 1.00f, 1.00f, 1.00f);   // pressed — almost-white cyan

        ImVec4* c = style.Colors;
        c[ImGuiCol_Text]                  = text;
        c[ImGuiCol_TextDisabled]          = text_dim;
        c[ImGuiCol_WindowBg]              = bg_window;
        c[ImGuiCol_ChildBg]               = bg_child;
        c[ImGuiCol_PopupBg]               = bg_popup;
        c[ImGuiCol_Border]                = border;
        c[ImGuiCol_BorderShadow]          = border_shadow;

        c[ImGuiCol_FrameBg]               = bg_frame;
        c[ImGuiCol_FrameBgHovered]        = bg_frame_hov;
        c[ImGuiCol_FrameBgActive]         = bg_frame_act;

        c[ImGuiCol_TitleBg]               = bg_window;
        c[ImGuiCol_TitleBgActive]         = bg_window;
        c[ImGuiCol_TitleBgCollapsed]      = bg_window;
        c[ImGuiCol_MenuBarBg]             = bg_window;

        c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(1, 1, 1, 0.14f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(1, 1, 1, 0.22f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(1, 1, 1, 0.30f);

        c[ImGuiCol_CheckMark]             = accent;
        c[ImGuiCol_SliderGrab]            = accent;
        c[ImGuiCol_SliderGrabActive]      = accent_act;
        c[ImGuiCol_Button]                = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(1, 1, 1, 0.14f);
        c[ImGuiCol_ButtonActive]          = accent;

        c[ImGuiCol_Header]                = ImVec4(1, 1, 1, 0.06f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(1, 1, 1, 0.10f);
        c[ImGuiCol_HeaderActive]          = ImVec4(1, 1, 1, 0.14f);

        c[ImGuiCol_Separator]             = border;
        c[ImGuiCol_SeparatorHovered]      = accent_hov;
        c[ImGuiCol_SeparatorActive]       = accent_act;

        c[ImGuiCol_ResizeGrip]            = ImVec4(1, 1, 1, 0.08f);
        c[ImGuiCol_ResizeGripHovered]     = accent_hov;
        c[ImGuiCol_ResizeGripActive]      = accent_act;

        c[ImGuiCol_Tab]                   = bg_child;
        c[ImGuiCol_TabHovered]            = ImVec4(1, 1, 1, 0.10f);
        c[ImGuiCol_TabActive]             = ImVec4(1, 1, 1, 0.14f);
        c[ImGuiCol_TabUnfocused]          = bg_child;
        c[ImGuiCol_TabUnfocusedActive]    = ImVec4(1, 1, 1, 0.08f);

        c[ImGuiCol_DragDropTarget]        = accent;
        c[ImGuiCol_NavHighlight]          = accent;
    }
} // namespace lumen
