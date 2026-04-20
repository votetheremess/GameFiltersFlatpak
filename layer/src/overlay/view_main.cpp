#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"
#include "effects/params/effect_param.hpp"
#include "logger.hpp"
#include "profile_manager.hpp"
#include "theme_gff.hpp"

#include <string>

#include "imgui/imgui.h"

namespace vkBasalt
{
    namespace
    {
        constexpr const char* kEffectName  = "gff_pipeline";
        constexpr float       kLabelColumn = 140.0f;
        constexpr float       kResetWidth  = 60.0f;    // trailing reset button width
        constexpr float       kCardRounding = 10.0f;
        constexpr ImVec2      kCardPadding  = ImVec2(14.0f, 12.0f);
        constexpr ImVec4      kCardBg       = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);

        void beginCard(const char* id)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kCardBg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kCardRounding);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, kCardPadding);
            ImGuiChildFlags flags = ImGuiChildFlags_AutoResizeY
                                  | ImGuiChildFlags_Borders
                                  | ImGuiChildFlags_AlwaysUseWindowPadding;
            ImGui::BeginChild(id, ImVec2(-FLT_MIN, 0.0f), flags);
        }

        void endCard()
        {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        void sectionHeader(const char* title)
        {
            ImFont* bold = gff::boldFont();
            if (bold)
                ImGui::PushFont(bold);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.92f));
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            if (bold)
                ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
        }

        void applyFloatEverywhere(EffectRegistry* reg,
                                  ImGuiOverlay*   overlay,
                                  const char*     key,
                                  float           value)
        {
            reg->setParameterValue(kEffectName, key, value);
            if (auto* cfg = reg->getConfig())
                cfg->setOverride(key, floatToString(value));
            overlay->markDirty();
            // Persist to the active per-game profile file on every change.
            // The .conf file is <1 KiB so this is faster than the debounce
            // window; no queueing/batching needed.
            gff::saveActiveProfile(reg);
        }

        void renderProfileTabs(EffectRegistry* reg, ImGuiOverlay* overlay)
        {
            const auto& pm = gff::state();

            // Game name label on its own line, then three equally-sized
            // profile buttons beneath it. Active profile is drawn with the
            // cyan accent button colour so it's visually selected.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.65f));
            ImGui::Text("Game: %s", pm.gameName.empty() ? "default" : pm.gameName.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

            const float avail   = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float btnW    = (avail - spacing * 2.0f) / 3.0f;

            const ImVec4 activeCol    = ImVec4(0.46f, 0.76f, 1.00f, 0.85f); // same cyan as slider grab
            const ImVec4 activeHovCol = ImVec4(0.55f, 0.82f, 1.00f, 0.95f);

            for (int i = 1; i <= 3; ++i)
            {
                if (i > 1)
                    ImGui::SameLine();

                bool isActive = (pm.activeProfile == i);
                if (isActive)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,        activeCol);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeHovCol);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  activeCol);
                }

                ImGui::PushID(i);
                std::string label = "Profile " + std::to_string(i);
                if (ImGui::Button(label.c_str(), ImVec2(btnW, 0.0f)) && !isActive)
                    gff::switchProfile(i, reg, overlay);
                ImGui::PopID();

                if (isActive)
                    ImGui::PopStyleColor(3);
            }
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
        }

        // Single slider row — label column, slider filling middle, reset
        // button at the right. Reset sets the value back to the effect's
        // registered default (0 for every Nff param — matches Nvidia's
        // neutral baseline).
        void sliderRow(EffectRegistry* reg,
                       ImGuiOverlay*   overlay,
                       const char*     label,
                       const char*     key,
                       const char*     fmt = "%.0f")
        {
            auto* param = reg->getParameter(kEffectName, key);
            if (!param)
                return;
            auto* fp = dynamic_cast<FloatParam*>(param);
            if (!fp)
                return;

            ImGui::PushID(key);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(kLabelColumn);

            // Slider takes the full remaining width minus the reset button
            // column. Negative widths are fractions-from-right in ImGui;
            // we compute explicitly so the reset button has stable width.
            float avail     = ImGui::GetContentRegionAvail().x;
            float spacing   = ImGui::GetStyle().ItemSpacing.x;
            float sliderW   = avail - kResetWidth - spacing;
            ImGui::SetNextItemWidth(sliderW > 50.0f ? sliderW : 50.0f);

            float v = fp->value;
            if (ImGui::SliderFloat("##v", &v, fp->minValue, fp->maxValue, fmt))
                applyFloatEverywhere(reg, overlay, key, v);

            ImGui::SameLine();
            const bool atDefault = (fp->value == fp->defaultValue);
            ImGui::BeginDisabled(atDefault);
            if (ImGui::Button("Reset", ImVec2(kResetWidth, 0.0f)))
                applyFloatEverywhere(reg, overlay, key, fp->defaultValue);
            ImGui::EndDisabled();
            if (!atDefault && ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset to %.0f", fp->defaultValue);

            ImGui::PopID();
        }
    } // namespace

    void ImGuiOverlay::renderMainView(const KeyboardState& /*keyboard*/)
    {
        if (!pEffectRegistry)
            return;

        // Profile selector strip at the very top — three slots per game,
        // state stored in ~/.config/game-filters-flatpak/games/<exe>/.
        renderProfileTabs(pEffectRegistry, this);

        // Card layout mirrors Nvidia Freestyle's current Nvidia App UI so
        // users can paste presets 1:1. Nvidia dropped the standalone
        // "Brightness" slider some time ago — Exposure covers the same
        // space with a more photographic semantic, so we follow suit.

        sectionHeader("Brightness / Contrast");
        beginCard("##BCCard");
        sliderRow(pEffectRegistry, this, "Exposure",   "gff.exposure");
        sliderRow(pEffectRegistry, this, "Contrast",   "gff.contrast");
        sliderRow(pEffectRegistry, this, "Highlights", "gff.highlights");
        sliderRow(pEffectRegistry, this, "Shadows",    "gff.shadows");
        sliderRow(pEffectRegistry, this, "Gamma",      "gff.gamma");
        endCard();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        sectionHeader("Color");
        beginCard("##ColorCard");
        sliderRow(pEffectRegistry, this, "Tint Color",     "gff.tintColor", "%.0f\xc2\xb0");
        sliderRow(pEffectRegistry, this, "Tint Intensity", "gff.tintIntensity");
        sliderRow(pEffectRegistry, this, "Temperature",    "gff.temperature");
        sliderRow(pEffectRegistry, this, "Vibrance",       "gff.vibrance");
        endCard();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        sectionHeader("Details");
        beginCard("##DetailsCard");
        sliderRow(pEffectRegistry, this, "Sharpen",    "gff.sharpen");
        sliderRow(pEffectRegistry, this, "Clarity",    "gff.clarity");
        sliderRow(pEffectRegistry, this, "HDR Toning", "gff.hdrToning");
        sliderRow(pEffectRegistry, this, "Bloom",      "gff.bloom");
        endCard();

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        // Vignette and Black & White are separate filter cards in Nvidia's
        // UI (each with their own enable toggle); we group them here under
        // a single "Effects" card since our model has one shared effect
        // and users just set the slider to 0 to disable.
        sectionHeader("Effects");
        beginCard("##EffectsCard");
        sliderRow(pEffectRegistry, this, "Vignette",      "gff.vignette");
        sliderRow(pEffectRegistry, this, "Black & White", "gff.bwIntensity");
        endCard();
    }
} // namespace vkBasalt
