// Lumen — tonal grading pass.
// Fuses the five Brightness/Contrast sliders (Exposure, Contrast,
// Shadows, Highlights, Gamma) into one shader. They must run together
// because the math has no HDR float headroom between intermediate
// stages — splitting Shadows/Highlights from Gamma from Exposure
// reintroduces the clamp-then-shape problem the bundled form avoids.
//
// Math is ported from the leaked Nvidia Adjustment.yfx (NVCAMERA folder,
// 470.05 driver beta). Sign convention follows user-observed Nvidia
// presets:
//   + shadows      = DARKER darks (Nvidia, opposite of Lightroom "lift")
//   - shadows      = BRIGHTER darks
//   + dark_shadows = DARKER deepest darks (< ~8% luma, OLED-crush range)
//   - dark_shadows = BRIGHTER deepest darks
//   + highlights   = recovery (darken brights, Lightroom convention)
//   + gamma        = brighter midtones (Nvidia + Lightroom agree)
//
// Color space: gamma-encoded sRGB (UNORM image views forced). Rec.601
// luma weights.

#version 450

layout(set = 0, binding = 0) uniform sampler2D img;

layout(constant_id = 0) const float u_exposure     = 0.0;  // [-100, 100]
layout(constant_id = 1) const float u_contrast     = 0.0;  // [-100, 100]
layout(constant_id = 2) const float u_highlights   = 0.0;  // [-100, 100]
layout(constant_id = 3) const float u_shadows      = 0.0;  // [-100, 100]
layout(constant_id = 4) const float u_gamma        = 0.0;  // [-100, 100]
layout(constant_id = 5) const float u_dark_shadows = 0.0;  // [-100, 100]

layout(location = 0) in  vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);
float luma(vec3 c) { return dot(c, LUMA); }

// Adjustment — Lightroom-style masked-pow curves for shadows/highlights,
// pow-based gamma, multiplicative exposure last. Sign convention follows
// Nvidia presets: +shadows darkens darks, +highlights recovers brights,
// +gamma brightens midtones. See the file header above for the full
// sign-convention table.
vec3 applyAdjustment(vec3 c)
{
    float l             = luma(c);
    float shadowMask    = 1.0 - smoothstep(0.0, 0.5, l);
    float highlightMask = smoothstep(0.5, 1.0, l);

    float highlights = u_highlights * 0.01;
    float shadows    = u_shadows    * 0.01;
    float gammaV     = u_gamma      * 0.01;

    float shadowExp    = exp2(shadows    * 0.7);
    float highlightExp = exp2(highlights * 0.7);
    vec3  shadowed     = pow(max(c, 0.0), vec3(shadowExp));
    vec3  recovered    = pow(max(c, 0.0), vec3(highlightExp));
    c = mix(c, shadowed,  shadowMask);
    c = mix(c, recovered, highlightMask);

    if (shadows < 0.0)
    {
        float lAfter   = luma(c);
        float deepMask = 1.0 - smoothstep(0.0, 0.08, lAfter);
        float deepExp  = exp2(shadows * 0.30);
        vec3  deepLift = pow(max(c, 0.0), vec3(deepExp));
        c = mix(c, deepLift, deepMask);
    }

    // Independent "Deep Shadows" knob — affects luma < 0.08 regardless
    // of where the main Shadows slider sits. Photographers call this
    // range "blacks" (distinct from shadows); on OLED panels it's the
    // near-off-pixel band that the main Shadows curve barely touches,
    // so users often want a separate control to crush or lift it.
    // Uses the same scale (exp2(s * 0.7)) as main Shadows so the feel
    // at ±100 is comparable, and stacks additively with the lift above
    // when the user wants extreme black-level changes.
    float darkShadows = u_dark_shadows * 0.01;
    if (darkShadows != 0.0)
    {
        float lDeep    = luma(c);
        float deepMask2 = 1.0 - smoothstep(0.0, 0.08, lDeep);
        float deepExp2  = exp2(darkShadows * 0.7);
        vec3  deepCurve = pow(max(c, 0.0), vec3(deepExp2));
        c = mix(c, deepCurve, deepMask2);
    }

    float gammaExp = exp2(-gammaV);
    c = pow(max(c, 0.0), vec3(gammaExp));

    float exposureV = exp2(u_exposure * 0.005);
    return c * exposureV;
}

vec3 applyContrast(vec3 c)
{
    float s      = (u_contrast + 100.0) * 0.005;
    float factor = exp(mix(log(0.5), log(2.0), s));
    return (c - 0.5) * factor + 0.5;
}

void main()
{
    vec4 src = texture(img, textureCoord);
    vec3 c   = src.rgb;

    c = applyAdjustment(c);
    c = applyContrast  (c);

    fragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
