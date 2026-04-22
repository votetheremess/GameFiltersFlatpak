#ifndef EFFECT_LUMEN_STYLISTIC_HPP_INCLUDED
#define EFFECT_LUMEN_STYLISTIC_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // Stylistic / mood pass of the Lumen chain — Black & White
    // desaturation and radial Vignette.
    class LumenStylisticEffect : public SimpleEffect
    {
    public:
        LumenStylisticEffect(LogicalDevice*       pLogicalDevice,
                           VkFormat             format,
                           VkExtent2D           imageExtent,
                           std::vector<VkImage> inputImages,
                           std::vector<VkImage> outputImages,
                           Config*              pConfig);
        ~LumenStylisticEffect();
    };
} // namespace vkBasalt

#endif // EFFECT_LUMEN_STYLISTIC_HPP_INCLUDED
