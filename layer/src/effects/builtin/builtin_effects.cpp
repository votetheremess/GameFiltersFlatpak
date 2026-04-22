#include "builtin_effects.hpp"

#include "effect_cas.hpp"
#include "effect_dls.hpp"
#include "effect_fxaa.hpp"
#include "effect_smaa.hpp"
#include "effect_deband.hpp"
#include "effect_lut.hpp"
#include "effect_lumen_local.hpp"
#include "effect_lumen_tonal.hpp"
#include "effect_lumen_color.hpp"
#include "effect_lumen_stylistic.hpp"

namespace vkBasalt
{
    const BuiltInEffects& BuiltInEffects::instance()
    {
        static BuiltInEffects registry;
        return registry;
    }

    bool BuiltInEffects::isBuiltIn(const std::string& typeName) const
    {
        return effects.find(typeName) != effects.end();
    }

    const BuiltInEffectDef* BuiltInEffects::getDef(const std::string& typeName) const
    {
        auto it = effects.find(typeName);
        return (it != effects.end()) ? &it->second : nullptr;
    }

    std::vector<std::string> BuiltInEffects::getTypeNames() const
    {
        std::vector<std::string> names;
        names.reserve(effects.size());
        for (const auto& [name, def] : effects)
            names.push_back(name);
        return names;
    }

    BuiltInEffects::BuiltInEffects()
    {
        // CAS - Contrast Adaptive Sharpening
        effects["cas"] = {
            "cas",
            false,  // uses UNORM
            {
                {"casSharpness", "Sharpness", ParamType::Float, 0.4f, 0.0f, 1.0f}
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<CasEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // DLS - Denoised Luma Sharpening
        effects["dls"] = {
            "dls",
            false,  // uses UNORM
            {
                {"dlsSharpness", "Sharpness", ParamType::Float, 0.5f, 0.0f, 1.0f},
                {"dlsDenoise", "Denoise", ParamType::Float, 0.17f, 0.0f, 1.0f}
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<DlsEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // FXAA - Fast Approximate Anti-Aliasing
        effects["fxaa"] = {
            "fxaa",
            true,   // uses SRGB
            {
                {"fxaaQualitySubpix", "Quality Subpix", ParamType::Float, 0.75f, 0.0f, 1.0f},
                {"fxaaQualityEdgeThreshold", "Edge Threshold", ParamType::Float, 0.125f, 0.0f, 0.5f},
                {"fxaaQualityEdgeThresholdMin", "Edge Threshold Min", ParamType::Float, 0.0312f, 0.0f, 0.1f}
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<FxaaEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // SMAA - Subpixel Morphological Anti-Aliasing
        effects["smaa"] = {
            "smaa",
            false,  // uses UNORM
            {
                {"smaaThreshold", "Threshold", ParamType::Float, 0.05f, 0.0f, 0.5f},
                {"smaaMaxSearchSteps", "Max Search Steps", ParamType::Int, 0, 0, 0, 32, 0, 112},
                {"smaaMaxSearchStepsDiag", "Max Search Steps Diag", ParamType::Int, 0, 0, 0, 16, 0, 20},
                {"smaaCornerRounding", "Corner Rounding", ParamType::Int, 0, 0, 0, 25, 0, 100}
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<SmaaEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // Deband - Color banding reduction
        effects["deband"] = {
            "deband",
            false,  // uses UNORM
            {
                {"debandAvgdiff", "Avg Diff", ParamType::Float, 3.4f, 0.0f, 255.0f},
                {"debandMaxdiff", "Max Diff", ParamType::Float, 6.8f, 0.0f, 255.0f},
                {"debandMiddiff", "Mid Diff", ParamType::Float, 3.3f, 0.0f, 255.0f},
                {"debandRange", "Range", ParamType::Float, 16.0f, 1.0f, 64.0f},
                {"debandIterations", "Iterations", ParamType::Int, 0, 0, 0, 4, 1, 16}
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<DebandEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // LUT - 3D Color Lookup Table
        effects["lut"] = {
            "lut",
            false,  // uses UNORM
            {
                {"lutFile", "LUT File", ParamType::Float, 0.0f, 0.0f, 0.0f}  // Placeholder param
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<LutEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // GFF chain — four sibling effects that together replicate Nvidia
        // Freestyle's filter stack. Users add, remove, and reorder these
        // as independent passes. Each one operates on gamma-encoded sRGB
        // (UNORM image views forced in the effect constructor) to match
        // the leaked Nvidia .yfx math (Adjustment.yfx, Details.yfx — 470.05
        // driver beta). Slider ranges follow Nvidia's public scale so
        // Windows preset values paste in directly.

        // Local / spatial: Sharpen, Clarity, HDR Toning, Bloom.
        effects["lumen_local"] = {
            "lumen_local",
            false,  // uses UNORM — math calibrated in sRGB-encoded space
            {
                {"lumen.sharpen",   "Sharpen",    ParamType::Float, 0.0f, 0.0f, 100.0f},
                {"lumen.clarity",   "Clarity",    ParamType::Float, 0.0f, 0.0f, 100.0f},
                {"lumen.hdrToning", "HDR Toning", ParamType::Float, 0.0f, 0.0f, 100.0f},
                {"lumen.bloom",     "Bloom",      ParamType::Float, 0.0f, 0.0f, 100.0f},
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<LumenLocalEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // Tonal: Exposure, Contrast, Highlights, Shadows, Gamma, Deep Shadows.
        effects["lumen_tonal"] = {
            "lumen_tonal",
            false,
            {
                {"lumen.exposure",    "Exposure",     ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.contrast",    "Contrast",     ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.highlights",  "Highlights",   ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.shadows",     "Shadows",      ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.gamma",       "Gamma",        ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.darkShadows", "Deep Shadows", ParamType::Float, 0.0f, -100.0f, 100.0f},
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<LumenTonalEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // Color: Tint Color, Tint Intensity, Temperature, Vibrance.
        effects["lumen_color"] = {
            "lumen_color",
            false,
            {
                {"lumen.tintColor",     "Tint Color",     ParamType::Float, 0.0f,    0.0f, 360.0f},
                {"lumen.tintIntensity", "Tint Intensity", ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"lumen.temperature",   "Temperature",    ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"lumen.vibrance",      "Vibrance",       ParamType::Float, 0.0f, -100.0f, 100.0f},
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<LumenColorEffect>(dev, fmt, ext, in, out, cfg);
            }
        };

        // Stylistic: Vignette, Black & White.
        effects["lumen_stylistic"] = {
            "lumen_stylistic",
            false,
            {
                {"lumen.vignette",    "Vignette",      ParamType::Float, 0.0f, 0.0f, 100.0f},
                {"lumen.bwIntensity", "Black & White", ParamType::Float, 0.0f, 0.0f, 100.0f},
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<LumenStylisticEffect>(dev, fmt, ext, in, out, cfg);
            }
        };
    }

} // namespace vkBasalt
