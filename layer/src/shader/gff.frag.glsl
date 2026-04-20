// GameFiltersFlatpak — combined filter pipeline calibrated to Nvidia
// Freestyle / Game Filters. Math is ported from the leaked Nvidia
// Adjustment.yfx and Details.yfx that shipped accidentally in the 470.05
// driver beta (NVCAMERA folder, plain HLSL). Sources:
//   - https://reshade.me/forum/shader-discussion/8873-nvidia-freestyle-adjustment-yfx-to-reshade
//   - https://reshade.me/forum/shader-suggestions/7664-can-someone-port-details-yfx-from-freestyle-to-reshade
//
// Color space: this shader operates on **gamma-encoded sRGB** values.
// effect_gff.cpp pins the input/output image views to the UNORM alias of
// the swapchain format so the hardware sampler does NOT auto-linearize
// on read and does NOT auto-encode sRGB on write. Rec.601 luma weights
// are used throughout (matching Freestyle); they are defined against
// gamma-encoded signal, not linear light.
//
// Slider ranges follow Nvidia's public scale so a Windows preset value
// can be pasted directly: bipolar ±100, unipolar 0..100, hue 0..360.
//
// Bloom, Clarity, and HDR Toning use a small in-shader blur instead of
// Freestyle's multi-pass downsampled gaussian; the math/blend matches
// but the kernel is smaller. v2 will promote those to a multi-pass
// pipeline using Vulkan render-pass chaining.
//
// Shader license: MIT.

#version 450

layout(set = 0, binding = 0) uniform sampler2D img;

// --- Brightness / Contrast ---------------------------------------------
layout(constant_id = 0)  const float u_exposure       =   0.0;  // [-100, 100]
layout(constant_id = 1)  const float u_contrast       =   0.0;  // [-100, 100]
layout(constant_id = 2)  const float u_highlights     =   0.0;  // [-100, 100]
layout(constant_id = 3)  const float u_shadows        =   0.0;  // [-100, 100]
layout(constant_id = 4)  const float u_gamma          =   0.0;  // [-100, 100]

// --- Color -------------------------------------------------------------
layout(constant_id = 5)  const float u_tint_color     =   0.0;  // [0, 360] hue deg
layout(constant_id = 6)  const float u_tint_intensity =   0.0;  // [0, 100]
layout(constant_id = 7)  const float u_temperature    =   0.0;  // [-100, 100]
layout(constant_id = 8)  const float u_vibrance       =   0.0;  // [-100, 100]

// --- Details -----------------------------------------------------------
layout(constant_id = 9)  const float u_sharpen        =   0.0;  // [0, 100]
layout(constant_id = 10) const float u_clarity        =   0.0;  // [0, 100]
layout(constant_id = 11) const float u_hdr_toning     =   0.0;  // [0, 100]
layout(constant_id = 12) const float u_bloom          =   0.0;  // [0, 100]

// --- Other -------------------------------------------------------------
layout(constant_id = 13) const float u_vignette       =   0.0;  // [0, 100]
layout(constant_id = 14) const float u_bw_intensity   =   0.0;  // [0, 100]

layout(location = 0) in  vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

// Rec.601 luma weights — what Freestyle uses. Defined against gamma-encoded
// (sRGB) signal. Rec.709 (0.2126/0.7152/0.0722) would be correct for linear
// light, but our pipeline runs in sRGB-encoded space.
const vec3 LUMA = vec3(0.299, 0.587, 0.114);

float luma(vec3 c) { return dot(c, LUMA); }

vec3 hueToRgb(float h)
{
    h = fract(h);
    vec3 k = vec3(0.0, 2.0 / 3.0, 1.0 / 3.0);
    vec3 p = abs(fract(vec3(h) + k) * 6.0 - 3.0);
    return clamp(p - 1.0, 0.0, 1.0);
}

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// --- Neighbor-sampling stages (read original `img`, must run before
//     per-pixel grading mutates the local color) -------------------------

// Sharpen — 4-tap box-blur unsharp, luma-only signed delta added back to
// every channel. Matches Details.yfx: `diff = dot(c - blur, Rec601);
// diff = clamp(diff, -k, k); out = c + diff * s` with k slider-driven.
vec3 applySharpen(vec2 uv, vec3 center, vec2 px)
{
    float s = u_sharpen * 0.01;
    if (s <= 0.0) return center;
    vec3 blur = 0.25 * (
          texture(img, uv + vec2( 1.0,  0.0) * px).rgb
        + texture(img, uv + vec2(-1.0,  0.0) * px).rgb
        + texture(img, uv + vec2( 0.0,  1.0) * px).rgb
        + texture(img, uv + vec2( 0.0, -1.0) * px).rgb);
    float diff   = dot(center - blur, LUMA);
    float clampR = mix(0.25, 0.6, s);
    diff = clamp(diff, -clampR, clampR);
    return center + vec3(diff * s);
}

// Clarity — luma-only midtone unsharp at wider radius. Freestyle does
// this on a multi-scale blur pyramid; v1 approximates with a single
// 5-tap luma blur at radius 3. Add the signed delta back to all channels
// so chroma is preserved.
vec3 applyClarity(vec2 uv, vec3 center, vec2 px)
{
    float s = u_clarity * 0.01;
    if (s <= 0.0) return center;
    float r       = 3.0;
    float centerL = luma(center);
    float blurL   = centerL
                  + luma(texture(img, uv + vec2( r,  0.0) * px).rgb)
                  + luma(texture(img, uv + vec2(-r,  0.0) * px).rgb)
                  + luma(texture(img, uv + vec2( 0.0,  r) * px).rgb)
                  + luma(texture(img, uv + vec2( 0.0, -r) * px).rgb);
    blurL *= 0.2;
    float delta = (centerL - blurL) * s;
    // Bound the delta by the per-channel headroom left on `center` so no
    // channel crosses 0.0 or 1.0. Without this, bright edge pixels overshoot
    // past 1.0, the final main() clamp turns them into pure white, and the
    // effect stacks as a milky "white haze" across highlights.
    float maxC         = max(max(center.r, center.g), center.b);
    float minC         = min(min(center.r, center.g), center.b);
    float headroomUp   = 1.0 - maxC;
    float headroomDown = minC;
    if (delta > 0.0)
        delta = min(delta, headroomUp);
    else
        delta = max(delta, -headroomDown);
    return center + vec3(delta);
}

// HDR Toning — Details.yfx ports it as a *modified soft-light blend*
// between the original luma and a blurred luma, NOT a tone-mapper.
// Formula: sqrt(a) * mix(sqrt(a) * (2ab - a - 2b + 2),
//                        2 * sqrt(a) * b - 2b + 1,
//                        step(0.5, b))
// where a = original luma, b = blurred luma.
vec3 applyHdrToning(vec2 uv, vec3 center, vec2 px)
{
    float s = u_hdr_toning * 0.01;
    if (s <= 0.0) return center;
    float r = 2.0;
    float a = luma(center);
    float b = 0.25 * (
          luma(texture(img, uv + vec2( r,  0.0) * px).rgb)
        + luma(texture(img, uv + vec2(-r,  0.0) * px).rgb)
        + luma(texture(img, uv + vec2( 0.0,  r) * px).rgb)
        + luma(texture(img, uv + vec2( 0.0, -r) * px).rgb));
    float sqrta = sqrt(max(a, 0.0));
    float lo    = sqrta * (2.0 * a * b - a - 2.0 * b + 2.0);
    float hi    = 2.0 * sqrta * b - 2.0 * b + 1.0;
    float toned = sqrta * mix(lo, hi, step(0.5, b));
    float scale = toned / max(a, 1e-4);
    return mix(center, center * scale, s);
}

// Bloom — Details.yfx screen-blends the image with its own large blur:
// out = 1 - (1 - c) * (1 - blur * s). v1 uses a 5x5 weighted gaussian
// at radius 4; v2 will use a true multi-pass downsample pyramid.
vec3 applyBloom(vec2 uv, vec3 center, vec2 px)
{
    float s = u_bloom * 0.01;
    if (s <= 0.0) return center;
    vec3 blur = vec3(0.0);
    float wsum = 0.0;
    for (int i = -2; i <= 2; ++i)
    {
        for (int j = -2; j <= 2; ++j)
        {
            float w = exp(-float(i * i + j * j) * 0.25);
            blur += w * texture(img, uv + vec2(float(i), float(j)) * 4.0 * px).rgb;
            wsum += w;
        }
    }
    blur /= wsum;
    return 1.0 - (1.0 - center) * (1.0 - blur * s);
}

// --- Per-pixel grading stages ------------------------------------------

// Adjustment — Lightroom-style chain. The leaked Adjustment.yfx fuses
// Exposure / Highlights / Shadows / Gamma into a single per-channel
// `pow(saturate(c * exposureV), exp2(...))`, but that formula assumes
// Nvidia's HDR float intermediates: values pushed >1 by exposure survive
// into the highlight-recovery pow stage. Our 8-bit UNORM single-pass
// shader has no such headroom, so we restructure as offsets-then-gamma-
// then-exposure with no intermediate clamp.
//
// Sign convention (asymmetric, matches Nvidia preset expectations):
//   + highlights = recovery (darken brights) — Lightroom convention
//   - highlights = brighten brights
//   + shadows    = darker shadows           — user-observed Nvidia behavior
//   - shadows    = brighter shadows (lift)
//   + gamma      = brighter midtones         — Nvidia / Lightroom convention
vec3 applyAdjustment(vec3 c)
{
    float l             = luma(c);
    float shadowMask    = 1.0 - smoothstep(0.0, 0.5, l);
    float highlightMask = smoothstep(0.5, 1.0, l);

    float highlights = u_highlights * 0.01;   // [-1, 1]
    float shadows    = u_shadows    * 0.01;   // [-1, 1]
    float gammaV     = u_gamma      * 0.01;   // [-1, 1]

    // Shadows / highlights as *masked gamma curves*, not additive offsets.
    // Additive lift adds a constant to dark pixels — on a predominantly
    // dark scene that reads as a semi-transparent grey overlay, because
    // pure black (0) is pushed to a visible grey. A power curve pins 0→0
    // and 1→1 while reshaping the interior, so deep blacks stay jet black
    // and only the dark-midtones curve up. Sign convention unchanged:
    // +shadows/+highlights darken, -shadows/-highlights brighten.
    float shadowExp    = exp2(shadows    * 0.7);   // ±0.7 → ~×1.62 / ~×0.62
    float highlightExp = exp2(highlights * 0.7);
    vec3  shadowed     = pow(max(c, 0.0), vec3(shadowExp));
    vec3  recovered    = pow(max(c, 0.0), vec3(highlightExp));
    c = mix(c, shadowed,  shadowMask);
    c = mix(c, recovered, highlightMask);

    // Extra lift for the very deepest darks only. The main shadow pow is
    // tuned to feel right at mid-shadows (luma ~0.2–0.4); on OLED the
    // bottom ~8% of luma still reads as off-pixel black after that.
    // A second, narrow pow applies only when the user is lifting shadows
    // (shadows < 0) and only where post-shadow luma is < ~0.08. Using pow
    // (not add) keeps pure black at 0 and avoids the grey-haze look the
    // additive version produced.
    if (shadows < 0.0)
    {
        float lAfter   = luma(c);
        float deepMask = 1.0 - smoothstep(0.0, 0.08, lAfter);
        float deepExp  = exp2(shadows * 0.30);
        vec3  deepLift = pow(max(c, 0.0), vec3(deepExp));
        c = mix(c, deepLift, deepMask);
    }

    // Gamma — per-pixel power on midtones. Negate so +gamma shrinks the
    // exponent below 1.0 (brighter midtones) and -gamma grows it (darker),
    // matching Nvidia's public direction.
    float gammaExp = exp2(-gammaV);    // ±100 → ÷2 / ×2
    c = pow(max(c, 0.0), vec3(gammaExp));

    // Exposure last so highlight recovery has already brought brights
    // into safe range. ±0.5 stop is gentler than Lightroom's ±5 EV; we
    // don't have HDR headroom or a final tonemap to recover overshoot.
    float exposureV = exp2(u_exposure * 0.005);  // ±100 → ±0.5 stop
    return c * exposureV;
}

// Contrast — Adjustment.yfx form `factor = exp(mix(ln 0.2, ln 5.0, s))`
// reaches 5× at +100, which assumes HDR float headroom. In our UNORM
// pipeline that pushes mid-bright pixels (~0.85) to ~1.0 at moderate
// slider values. Tamed range: 0.5× to 2×.
vec3 applyContrast(vec3 c)
{
    float s      = (u_contrast + 100.0) * 0.005;
    float factor = exp(mix(log(0.5), log(2.0), s));
    return (c - 0.5) * factor + 0.5;
}

// Temperature — Nvidia's exact math is not in the leak. Approximate with
// an axial warm/cool shift, capped at ±10% gain (~±2000K photographic).
vec3 applyTemperature(vec3 c)
{
    float t = u_temperature * 0.01;
    return c * vec3(1.0 + 0.10 * t, 1.0, 1.0 - 0.10 * t);
}

// Vibrance — Adjustment.yfx form: convert to HSV, then asymmetric curve
// on saturation: positive uses pow (boost), negative uses linear scale.
vec3 applyVibrance(vec3 c)
{
    float v = u_vibrance * 0.01;
    if (v == 0.0) return c;
    vec3 hsv = rgb2hsv(c);
    if (v > 0.0)
        hsv.y = pow(hsv.y, 1.0 / max(1.0 + v, 1e-3));
    else
        hsv.y = clamp(hsv.y * (1.0 + v), 0.0, 1.0);
    return hsv2rgb(hsv);
}

// Tint — multiply blend toward the chosen hue color (multiply matches
// Freestyle's typical "tint" feel; not in the leak).
vec3 applyTint(vec3 c)
{
    float i = u_tint_intensity * 0.01;
    if (i <= 0.0) return c;
    vec3 hueRGB = hueToRgb(u_tint_color / 360.0);
    return mix(c, c * hueRGB, i * 0.5);
}

vec3 applyBlackAndWhite(vec3 c)
{
    return mix(c, vec3(luma(c)), clamp(u_bw_intensity * 0.01, 0.0, 1.0));
}

vec3 applyVignette(vec3 c, vec2 uv)
{
    float a = u_vignette * 0.01;
    if (a <= 0.0) return c;
    vec2  centered = uv - 0.5;
    float d        = length(centered) * 1.4142;
    float fall     = smoothstep(0.35, 1.0, d);
    return c * (1.0 - fall * a * 0.6);
}

// --- main --------------------------------------------------------------

void main()
{
    vec4 src   = texture(img, textureCoord);
    vec3 c     = src.rgb;
    vec2 pixel = 1.0 / vec2(textureSize(img, 0));

    // Neighbor-sampling stages — all read the original `img` so they
    // cannot see each other's per-pixel mutations. Order within this
    // group does not matter for correctness, only for layered look.
    c = applySharpen   (textureCoord, c, pixel);
    c = applyClarity   (textureCoord, c, pixel);
    c = applyHdrToning (textureCoord, c, pixel);
    c = applyBloom     (textureCoord, c, pixel);

    // Per-pixel grading chain — Adjustment first (fused exp/shadows/
    // highlights/gamma), then contrast, then color, then stylistic.
    c = applyAdjustment      (c);
    c = applyContrast        (c);
    c = applyTemperature     (c);
    c = applyTint            (c);
    c = applyVibrance        (c);
    c = applyBlackAndWhite   (c);
    c = applyVignette        (c, textureCoord);

    fragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
