#ifndef EFFECT_LUMEN_TONAL_HPP_INCLUDED
#define EFFECT_LUMEN_TONAL_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // Tonal pass of the Lumen chain — Exposure, Contrast,
    // Highlights, Shadows, Gamma. All five are bundled in one shader
    // because the math has no HDR float headroom to split across passes.
    class LumenTonalEffect : public SimpleEffect
    {
    public:
        LumenTonalEffect(LogicalDevice*       pLogicalDevice,
                       VkFormat             format,
                       VkExtent2D           imageExtent,
                       std::vector<VkImage> inputImages,
                       std::vector<VkImage> outputImages,
                       Config*              pConfig);
        ~LumenTonalEffect();
    };
} // namespace vkBasalt

#endif // EFFECT_LUMEN_TONAL_HPP_INCLUDED
