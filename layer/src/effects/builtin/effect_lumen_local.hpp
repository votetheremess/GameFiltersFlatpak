#ifndef EFFECT_LUMEN_LOCAL_HPP_INCLUDED
#define EFFECT_LUMEN_LOCAL_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // Local / spatial filter pass of the Lumen chain — Sharpen,
    // Clarity, HDR Toning, Bloom. Reads Config for four lumen.* float params
    // and bakes them into specialization constants for lumen_local.frag.glsl.
    class LumenLocalEffect : public SimpleEffect
    {
    public:
        LumenLocalEffect(LogicalDevice*       pLogicalDevice,
                       VkFormat             format,
                       VkExtent2D           imageExtent,
                       std::vector<VkImage> inputImages,
                       std::vector<VkImage> outputImages,
                       Config*              pConfig);
        ~LumenLocalEffect();
    };
} // namespace vkBasalt

#endif // EFFECT_LUMEN_LOCAL_HPP_INCLUDED
