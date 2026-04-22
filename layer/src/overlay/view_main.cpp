#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"
#include "effects/params/effect_param.hpp"
#include "logger.hpp"
#include "profile_manager.hpp"
#include "theme_lumen.hpp"

#include <algorithm>
#include <string>

#include "imgui/imgui.h"

namespace vkBasalt
{
    namespace
    {
        constexpr float  kLabelColumn  = 140.0f;
        constexpr float  kResetWidth   = 60.0f;   // trailing per-slider reset button
        constexpr float  kActionWidth  = 64.0f;   // per-card Up/Down/Remove/Add button
        constexpr float  kCardRounding = 10.0f;
        constexpr ImVec2 kCardPadding  = ImVec2(14.0f, 12.0f);
        constexpr ImVec4 kCardBg       = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);

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

        void boldText(const char* s, float alpha = 0.92f)
        {
            ImFont* bold = lumen::boldFont();
            if (bold)
                ImGui::PushFont(bold);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
            ImGui::TextUnformatted(s);
            ImGui::PopStyleColor();
            if (bold)
                ImGui::PopFont();
        }

        void mutedText(const char* s)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.55f));
            ImGui::TextUnformatted(s);
            ImGui::PopStyleColor();
        }

        void applyFloatEverywhere(EffectRegistry* reg,
                                  ImGuiOverlay*   overlay,
                                  const char*     effectName,
                                  const char*     key,
                                  float           value)
        {
            reg->setParameterValue(effectName, key, value);
            if (auto* cfg = reg->getConfig())
                cfg->setOverride(key, floatToString(value));
            overlay->markDirty();
            // Persist to the active per-game profile file on every change.
            // The .conf file is <1 KiB so this is faster than the debounce
            // window; no queueing/batching needed.
            lumen::saveActiveProfile(reg);
        }

        void renderProfileTabs(EffectRegistry* reg, ImGuiOverlay* overlay)
        {
            const auto& pm = lumen::state();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 0.65f));
            ImGui::Text("Game: %s", pm.gameName.empty() ? "default" : pm.gameName.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

            const float avail   = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float btnW    = (avail - spacing * 2.0f) / 3.0f;

            const ImVec4 activeCol    = ImVec4(0.00f, 0.96f, 1.00f, 0.85f);  // #00F5FF electric cyan
            const ImVec4 activeHovCol = ImVec4(0.30f, 1.00f, 1.00f, 0.95f);  // brighter hover variant

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
                    lumen::switchProfile(i, reg, overlay);
                ImGui::PopID();

                if (isActive)
                    ImGui::PopStyleColor(3);
            }
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
        }

        // Single slider row — label column, slider filling middle, reset
        // button at the right. `effectName` is the card's owning effect
        // (e.g. "lumen_tonal") so the write routes to the right SPIR-V
        // specialization-constant set.
        void sliderRow(EffectRegistry*        reg,
                       ImGuiOverlay*          overlay,
                       const char*            effectName,
                       const lumen::CardSlider& s)
        {
            auto* param = reg->getParameter(effectName, s.key);
            if (!param)
                return;
            auto* fp = dynamic_cast<FloatParam*>(param);
            if (!fp)
                return;

            ImGui::PushID(s.key);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.label);
            ImGui::SameLine(kLabelColumn);

            float avail   = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float sliderW = avail - kResetWidth - spacing;
            ImGui::SetNextItemWidth(sliderW > 50.0f ? sliderW : 50.0f);

            float v = fp->value;
            if (ImGui::SliderFloat("##v", &v, fp->minValue, fp->maxValue, s.fmt))
                applyFloatEverywhere(reg, overlay, effectName, s.key, v);

            ImGui::SameLine();
            const bool atDefault = (fp->value == fp->defaultValue);
            ImGui::BeginDisabled(atDefault);
            if (ImGui::Button("Reset", ImVec2(kResetWidth, 0.0f)))
                applyFloatEverywhere(reg, overlay, effectName, s.key, fp->defaultValue);
            ImGui::EndDisabled();
            if (!atDefault && ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset to %.0f", fp->defaultValue);

            ImGui::PopID();
        }

        // Header row for an active card — bold title on the left, Up/Down/
        // Remove buttons right-aligned. `canUp`/`canDown` drive the
        // disabled-state of the movement buttons so the first and last
        // cards don't offer nonsensical moves.
        void renderActiveCardHeader(EffectRegistry*     reg,
                                    ImGuiOverlay*       overlay,
                                    const lumen::CardDef& card,
                                    bool                canUp,
                                    bool                canDown)
        {
            ImGui::PushID(card.effectName);

            const ImGuiStyle& style    = ImGui::GetStyle();
            const float       spacing  = style.ItemSpacing.x;
            const float       avail    = ImGui::GetContentRegionAvail().x;
            const float       buttonsW = kActionWidth * 3.0f + spacing * 2.0f;

            ImGui::AlignTextToFramePadding();
            boldText(card.title);
            ImGui::SameLine(avail - buttonsW);

            ImGui::BeginDisabled(!canUp);
            if (ImGui::Button("Up", ImVec2(kActionWidth, 0.0f)))
                lumen::moveCard(card.effectName, -1, reg, overlay);
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!canDown);
            if (ImGui::Button("Down", ImVec2(kActionWidth, 0.0f)))
                lumen::moveCard(card.effectName, +1, reg, overlay);
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(kActionWidth, 0.0f)))
                lumen::removeCard(card.effectName, reg, overlay);

            ImGui::PopID();
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
        }

        // One compact row per inactive card in the "Add filter" section:
        // muted title on the left, "Add" button right-aligned.
        void renderInactiveCardRow(EffectRegistry*     reg,
                                   ImGuiOverlay*       overlay,
                                   const lumen::CardDef& card)
        {
            ImGui::PushID(card.effectName);

            const float avail = ImGui::GetContentRegionAvail().x;

            ImGui::AlignTextToFramePadding();
            mutedText(card.title);
            ImGui::SameLine(avail - kActionWidth);
            if (ImGui::Button("Add", ImVec2(kActionWidth, 0.0f)))
                lumen::addCard(card.effectName, reg, overlay);

            ImGui::PopID();
        }

        const lumen::CardDef* findCard(const std::string& effectName)
        {
            for (const auto& c : lumen::cards())
                if (effectName == c.effectName)
                    return &c;
            return nullptr;
        }
    } // namespace

    void ImGuiOverlay::renderMainView(const KeyboardState& /*keyboard*/)
    {
        if (!pEffectRegistry)
            return;

        // Profile selector strip at the top — three slots per game, state
        // stored in ~/.config/lumen/games/<exe>/.
        renderProfileTabs(pEffectRegistry, this);

        const auto& chain    = pEffectRegistry->getSelectedEffects();
        const auto& allCards = lumen::cards();

        // Active-filters section. Renders active cards in chain order.
        // When nothing is active, shows a muted hint pointing users to
        // the "Add filter" section below — matching Nvidia Freestyle's
        // empty-state behavior where a new preset has no filters.
        boldText("Active filters");
        ImGui::Dummy(ImVec2(0.0f, 3.0f));

        if (chain.empty())
        {
            mutedText("No filters active. Add one below.");
        }
        else
        {
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const auto* card = findCard(chain[i]);
                if (!card)
                    continue;  // non-lumen effect in the chain — skip, not our UI

                const bool canUp   = (i > 0);
                const bool canDown = (i + 1 < chain.size());

                renderActiveCardHeader(pEffectRegistry, this, *card, canUp, canDown);
                beginCard((std::string("##card_") + card->effectName).c_str());
                for (const auto& s : card->sliders)
                    sliderRow(pEffectRegistry, this, card->effectName, s);
                endCard();

                if (i + 1 < chain.size())
                    ImGui::Dummy(ImVec2(0.0f, 6.0f));
            }
        }

        // Add-filter section. Lists canonical cards that aren't already in
        // the chain, in the canonical order. Hidden when all four cards
        // are active.
        std::vector<const lumen::CardDef*> inactive;
        for (const auto& card : allCards)
        {
            if (std::find(chain.begin(), chain.end(), std::string(card.effectName)) == chain.end())
                inactive.push_back(&card);
        }

        if (!inactive.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, 14.0f));
            boldText("Add filter");
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            beginCard("##addCard");
            for (size_t i = 0; i < inactive.size(); ++i)
            {
                if (i > 0)
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                renderInactiveCardRow(pEffectRegistry, this, *inactive[i]);
            }
            endCard();
        }
    }
} // namespace vkBasalt
