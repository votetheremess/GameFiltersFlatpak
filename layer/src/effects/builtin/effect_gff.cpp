#include "effect_gff.hpp"

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
        // Order must match the `constant_id = N` declarations in gff.frag.glsl.
        struct NffParams
        {
            // Brightness / Contrast
            float exposure;       // [-100, 100]
            float contrast;       // [-100, 100]
            float highlights;     // [-100, 100]
            float shadows;        // [-100, 100]
            float gamma;          // [-100, 100]
            // Color
            float tintColor;      // [   0, 360]
            float tintIntensity;  // [   0, 100]
            float temperature;    // [-100, 100]
            float vibrance;       // [-100, 100]
            // Details
            float sharpen;        // [   0, 100]
            float clarity;        // [   0, 100]
            float hdrToning;      // [   0, 100]
            float bloom;          // [   0, 100]
            // Other
            float vignette;       // [   0, 100]
            float bwIntensity;    // [   0, 100]
        };

        NffParams readParams(Config* pConfig)
        {
            return NffParams{
                pConfig->getOption<float>("gff.exposure",      0.0f),
                pConfig->getOption<float>("gff.contrast",      0.0f),
                pConfig->getOption<float>("gff.highlights",    0.0f),
                pConfig->getOption<float>("gff.shadows",       0.0f),
                pConfig->getOption<float>("gff.gamma",         0.0f),
                pConfig->getOption<float>("gff.tintColor",     0.0f),
                pConfig->getOption<float>("gff.tintIntensity", 0.0f),
                pConfig->getOption<float>("gff.temperature",   0.0f),
                pConfig->getOption<float>("gff.vibrance",      0.0f),
                pConfig->getOption<float>("gff.sharpen",       0.0f),
                pConfig->getOption<float>("gff.clarity",       0.0f),
                pConfig->getOption<float>("gff.hdrToning",     0.0f),
                pConfig->getOption<float>("gff.bloom",         0.0f),
                pConfig->getOption<float>("gff.vignette",      0.0f),
                pConfig->getOption<float>("gff.bwIntensity",   0.0f),
            };
        }
    } // namespace

    GffEffect::GffEffect(LogicalDevice*       pLogicalDevice,
                         VkFormat             format,
                         VkExtent2D           imageExtent,
                         std::vector<VkImage> inputImages,
                         std::vector<VkImage> outputImages,
                         Config*              pConfig)
    {
        static NffParams params;
        params = readParams(pConfig);

        vertexCode   = full_screen_triangle_vert;
        fragmentCode = gff_frag;

        constexpr size_t kParamCount = sizeof(NffParams) / sizeof(float);
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
        specInfo.dataSize      = sizeof(NffParams);
        specInfo.pData         = &params;

        pVertexSpecInfo   = nullptr;
        pFragmentSpecInfo = &specInfo;

        // Force UNORM image views so the hardware sampler does NOT auto-linearize on read,
        // and the shader output does NOT auto-encode sRGB on write. The leaked Nvidia
        // Freestyle .yfx shaders (Adjustment.yfx / Details.yfx, from the 470.05 driver)
        // are calibrated against gamma-encoded sRGB values directly and use Rec.601 luma
        // weights. Pinning to UNORM keeps math in the space Freestyle was tuned in,
        // regardless of whether the game's swapchain is *_SRGB or *_UNORM.
        const VkFormat unormView = convertToUNORM(format);
        Logger::info("gff_pipeline: swapchain format=" + convertToString(format)
                     + " using view format=" + convertToString(unormView));

        init(pLogicalDevice, format, imageExtent, inputImages, outputImages, pConfig, unormView);
    }

    GffEffect::~GffEffect() = default;
} // namespace vkBasalt
