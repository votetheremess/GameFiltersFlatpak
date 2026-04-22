#include "effect_lumen_tonal.hpp"

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
        // lumen_tonal.frag.glsl.
        struct LumenTonalParams
        {
            float exposure;     // [-100, 100]
            float contrast;     // [-100, 100]
            float highlights;   // [-100, 100]
            float shadows;      // [-100, 100]
            float gamma;        // [-100, 100]
            float darkShadows;  // [-100, 100]
        };

        LumenTonalParams readParams(Config* pConfig)
        {
            return LumenTonalParams{
                pConfig->getOption<float>("lumen.exposure",    0.0f),
                pConfig->getOption<float>("lumen.contrast",    0.0f),
                pConfig->getOption<float>("lumen.highlights",  0.0f),
                pConfig->getOption<float>("lumen.shadows",     0.0f),
                pConfig->getOption<float>("lumen.gamma",       0.0f),
                pConfig->getOption<float>("lumen.darkShadows", 0.0f),
            };
        }
    } // namespace

    LumenTonalEffect::LumenTonalEffect(LogicalDevice*       pLogicalDevice,
                                   VkFormat             format,
                                   VkExtent2D           imageExtent,
                                   std::vector<VkImage> inputImages,
                                   std::vector<VkImage> outputImages,
                                   Config*              pConfig)
    {
        static LumenTonalParams params;
        params = readParams(pConfig);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = lumen_tonal_frag;

        constexpr size_t kParamCount = sizeof(LumenTonalParams) / sizeof(float);
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
        specInfo.dataSize      = sizeof(LumenTonalParams);
        specInfo.pData         = &params;

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &specInfo;

        const VkFormat unormView = convertToUNORM(format);
        Logger::info("lumen_tonal: swapchain format=" + convertToString(format)
                     + " using view format=" + convertToString(unormView));

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig, unormView);
    }

    LumenTonalEffect::~LumenTonalEffect() = default;
} // namespace vkBasalt
