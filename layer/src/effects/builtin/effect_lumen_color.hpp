#ifndef EFFECT_LUMEN_COLOR_HPP_INCLUDED
#define EFFECT_LUMEN_COLOR_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // Color / chroma pass of the Lumen chain — Tint Color,
    // Tint Intensity, Temperature, Vibrance.
    class LumenColorEffect : public SimpleEffect
    {
    public:
        LumenColorEffect(LogicalDevice*       pLogicalDevice,
                       VkFormat             format,
                       VkExtent2D           imageExtent,
                       std::vector<VkImage> inputImages,
                       std::vector<VkImage> outputImages,
                       Config*              pConfig);
        ~LumenColorEffect();
    };
} // namespace vkBasalt

#endif // EFFECT_LUMEN_COLOR_HPP_INCLUDED
