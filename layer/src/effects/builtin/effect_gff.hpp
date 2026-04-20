#ifndef EFFECT_GFF_HPP_INCLUDED
#define EFFECT_GFF_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hpp"

#include "../effect_simple.hpp"
#include "config.hpp"

namespace vkBasalt
{
    // The combined v1 filter pipeline for GameFiltersFlatpak. Reads its
    // 8 float parameters from Config (gff.brightness, gff.contrast, ...)
    // and compiles them into specialization constants for the SPIR-V shader.
    //
    // TODO: switch from specialization constants to a push-constant block so
    // sliders can update without recompiling the pipeline (needed for the
    // live-drag experience).
    class GffEffect : public SimpleEffect
    {
    public:
        GffEffect(LogicalDevice*       pLogicalDevice,
                  VkFormat             format,
                  VkExtent2D           imageExtent,
                  std::vector<VkImage> inputImages,
                  std::vector<VkImage> outputImages,
                  Config*              pConfig);
        ~GffEffect();
    };
} // namespace vkBasalt

#endif // EFFECT_GFF_HPP_INCLUDED
