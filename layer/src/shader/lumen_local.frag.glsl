// Lumen — local / spatial filter pass.
// Implements the four filters that sample neighbor pixels: Sharpen,
// Clarity, HDR Toning, Bloom.
//
// Math is ported from the leaked Nvidia Freestyle Details.yfx (NVCAMERA
// folder, 470.05 driver beta). Sources:
//   - https://reshade.me/forum/shader-discussion/8873-nvidia-freestyle-adjustment-yfx-to-reshade
//   - https://reshade.me/forum/shader-suggestions/7664-can-someone-port-details-yfx-from-freestyle-to-reshade
//
// Color space: gamma-encoded sRGB (UNORM image views are forced in
// effect_lumen_local.cpp so the hardware sampler never auto-linearizes
// or re-encodes). Rec.601 luma weights match Freestyle's reference.
// Slider ranges follow Nvidia's public scale — bipolar ±100 or unipolar
// 0..100 — so Windows preset values paste in directly.

#version 450

layout(set = 0, binding = 0) uniform sampler2D img;

layout(constant_id = 0) const float u_sharpen    = 0.0;  // [0, 100]
layout(constant_id = 1) const float u_clarity    = 0.0;  // [0, 100]
layout(constant_id = 2) const float u_hdr_toning = 0.0;  // [0, 100]
layout(constant_id = 3) const float u_bloom      = 0.0;  // [0, 100]

layout(location = 0) in  vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);
float luma(vec3 c) { return dot(c, LUMA); }

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

void main()
{
    vec4 src   = texture(img, textureCoord);
    vec3 c     = src.rgb;
    vec2 pixel = 1.0 / vec2(textureSize(img, 0));

    c = applySharpen   (textureCoord, c, pixel);
    c = applyClarity   (textureCoord, c, pixel);
    c = applyHdrToning (textureCoord, c, pixel);
    c = applyBloom     (textureCoord, c, pixel);

    fragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
