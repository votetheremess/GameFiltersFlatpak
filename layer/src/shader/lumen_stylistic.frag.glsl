// Lumen — stylistic / mood pass.
// Black & White desaturation and radial Vignette. Traditionally applied
// near the end of a grading chain; in the multi-pass model the user can
// position this pass anywhere they like, with the understanding that
// placing it before Color makes Vibrance a no-op on a desaturated image.
//
// Color space: gamma-encoded sRGB (UNORM image views forced).

#version 450

layout(set = 0, binding = 0) uniform sampler2D img;

layout(constant_id = 0) const float u_vignette     = 0.0;  // [0, 100]
layout(constant_id = 1) const float u_bw_intensity = 0.0;  // [0, 100]

layout(location = 0) in  vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);
float luma(vec3 c) { return dot(c, LUMA); }

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

void main()
{
    vec4 src = texture(img, textureCoord);
    vec3 c   = src.rgb;

    c = applyBlackAndWhite(c);
    c = applyVignette     (c, textureCoord);

    fragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
