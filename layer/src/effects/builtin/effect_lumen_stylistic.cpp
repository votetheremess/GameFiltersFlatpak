#include "effect_lumen_stylistic.hpp"

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
        // lumen_stylistic.frag.glsl.
        struct LumenStylisticParams
        {
            float vignette;     // [0, 100]
            float bwIntensity;  // [0, 100]
        };

        LumenStylisticParams readParams(Config* pConfig)
        {
            return LumenStylisticParams{
                pConfig->getOption<float>("lumen.vignette",    0.0f),
                pConfig->getOption<float>("lumen.bwIntensity", 0.0f),
            };
        }
    } // namespace

    LumenStylisticEffect::LumenStylisticEffect(LogicalDevice*       pLogicalDevice,
                                           VkFormat             format,
                                           VkExtent2D           imageExtent,
                                           std::vector<VkImage> inputImages,
                                           std::vector<VkImage> outputImages,
                                           Config*              pConfig)
    {
        static LumenStylisticParams params;
        params = readParams(pConfig);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = lumen_stylistic_frag;

        constexpr size_t kParamCount = sizeof(LumenStylisticParams) / sizeof(float);
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
        specInfo.dataSize      = sizeof(LumenStylisticParams);
        specInfo.pData         = &params;

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &specInfo;

        const VkFormat unormView = convertToUNORM(format);
        Logger::info("lumen_stylistic: swapchain format=" + convertToString(format)
                     + " using view format=" + convertToString(unormView));

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig, unormView);
    }

    LumenStylisticEffect::~LumenStylisticEffect() = default;
} // namespace vkBasalt
