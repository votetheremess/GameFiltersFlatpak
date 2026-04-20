#include "builtin_effects.hpp"

#include "effect_cas.hpp"
#include "effect_dls.hpp"
#include "effect_fxaa.hpp"
#include "effect_smaa.hpp"
#include "effect_deband.hpp"
#include "effect_lut.hpp"
#include "effect_gff.hpp"

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

        // GFF Pipeline — combined filter pass that replicates Nvidia Freestyle's
        // public parameter semantics (ranges in -100..+100 or 0..100 so users
        // can paste values straight from their Windows presets). Shader is at
        // layer/src/shader/gff.frag.glsl; param normalization happens inside
        // the shader's main() body.
        //
        // Per-slider math is ported from the leaked Nvidia Freestyle .yfx files
        // shipped accidentally in the 470.05 driver beta — `Adjustment.yfx`
        // (Brightness/Contrast/Color block) and `Details.yfx` (Sharpen/Clarity/
        // HDR Toning/Bloom block). Those shaders operate on gamma-encoded sRGB
        // values directly with Rec.601 luma weights; `effect_gff.cpp` forces
        // UNORM image views so the hardware sampler never auto-linearizes.
        effects["gff_pipeline"] = {
            "gff_pipeline",
            false,  // uses UNORM — required: math is calibrated in sRGB-encoded space
            {
                // Brightness / Contrast (Nvidia's first filter card)
                {"gff.exposure",      "Exposure",       ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"gff.contrast",      "Contrast",       ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"gff.highlights",    "Highlights",     ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"gff.shadows",       "Shadows",        ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"gff.gamma",         "Gamma",          ParamType::Float, 0.0f, -100.0f, 100.0f},
                // Color (Nvidia's second filter card)
                {"gff.tintColor",     "Tint Color",     ParamType::Float, 0.0f,    0.0f, 360.0f},
                {"gff.tintIntensity", "Tint Intensity", ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"gff.temperature",   "Temperature",    ParamType::Float, 0.0f, -100.0f, 100.0f},
                {"gff.vibrance",      "Vibrance",       ParamType::Float, 0.0f, -100.0f, 100.0f},
                // Details (Nvidia's third filter card)
                {"gff.sharpen",       "Sharpen",        ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"gff.clarity",       "Clarity",        ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"gff.hdrToning",     "HDR Toning",     ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"gff.bloom",         "Bloom",          ParamType::Float, 0.0f,    0.0f, 100.0f},
                // Extra — standalone filters in Nvidia's UI, grouped here
                {"gff.vignette",      "Vignette",       ParamType::Float, 0.0f,    0.0f, 100.0f},
                {"gff.bwIntensity",   "Black & White",  ParamType::Float, 0.0f,    0.0f, 100.0f},
            },
            [](LogicalDevice* dev, VkFormat fmt, VkExtent2D ext,
               std::vector<VkImage> in, std::vector<VkImage> out, Config* cfg) {
                return std::make_shared<GffEffect>(dev, fmt, ext, in, out, cfg);
            }
        };
    }

} // namespace vkBasalt
