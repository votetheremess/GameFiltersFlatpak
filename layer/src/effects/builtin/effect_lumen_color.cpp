#include "effect_lumen_color.hpp"

#include <array>
#include <cstring>

#include "format.hpp"
#include "logger.hpp"
#include "shader_sources.hpp"
#include "util.hpp"

namespace vkBasalt
{
    namespace
    {
        // Order must match the `constant_id = N` declarations in
        // lumen_color.frag.glsl.
        struct LumenColorParams
        {
            float tintColor;      // [   0, 360] hue degrees
            float tintIntensity;  // [   0, 100]
            float temperature;    // [-100, 100]
            float vibrance;       // [-100, 100]
        };

        LumenColorParams readParams(Config* pConfig)
        {
            return LumenColorParams{
                pConfig->getOption<float>("lumen.tintColor",     0.0f),
                pConfig->getOption<float>("lumen.tintIntensity", 0.0f),
                pConfig->getOption<float>("lumen.temperature",   0.0f),
                pConfig->getOption<float>("lumen.vibrance",      0.0f),
            };
        }
    } // namespace

    LumenColorEffect::LumenColorEffect(LogicalDevice*       pLogicalDevice,
                                   VkFormat             format,
                                   VkExtent2D           imageExtent,
                                   std::vector<VkImage> inputImages,
                                   std::vector<VkImage> outputImages,
                                   Config*              pConfig)
    {
        static LumenColorParams params;
        params = readParams(pConfig);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = lumen_color_frag;

        constexpr size_t kParamCount = sizeof(LumenColorParams) / sizeof(float);
        static std::array<VkSpecializationMapEntry, kParamCount> entries = []() {
            std::array<VkSpecializationMapEntry, kParamCount> e{};
            for (uint32_t i = 0; i < e.size(); ++i)
            {
                e[i].constantID = i;
                e[i].offset     = i * sizeof(float);
                e[i].size       = sizeof(float);
            }
            return e;
        }();

        static VkSpecializationInfo specInfo{};
        specInfo.mapEntryCount = static_cast<uint32_t>(entries.size());
        specInfo.pMapEntries   = entries.data();
        specInfo.dataSize      = sizeof(LumenColorParams);
        specInfo.pData         = &params;

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &specInfo;

        const VkFormat unormView = convertToUNORM(format);
        Logger::info("lumen_color: swapchain format=" + convertToString(format)
                     + " using view format=" + convertToString(unormView));

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig, unormView);
    }

    LumenColorEffect::~LumenColorEffect() = default;
} // namespace vkBasalt
