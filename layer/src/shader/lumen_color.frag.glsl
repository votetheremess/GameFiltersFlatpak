// Lumen — color / chroma pass.
// Tint Color, Tint Intensity, Temperature, Vibrance. All operate on
// hue / saturation axes; no neighbor sampling, no tonal interaction
// beyond what the caller has already baked in via preceding passes.
//
// Color space: gamma-encoded sRGB (UNORM image views forced).

#version 450

layout(set = 0, binding = 0) uniform sampler2D img;

layout(constant_id = 0) const float u_tint_color     = 0.0;  // [0, 360] hue deg
layout(constant_id = 1) const float u_tint_intensity = 0.0;  // [0, 100]
layout(constant_id = 2) const float u_temperature    = 0.0;  // [-100, 100]
layout(constant_id = 3) const float u_vibrance       = 0.0;  // [-100, 100]

layout(location = 0) in  vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

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

vec3 applyTemperature(vec3 c)
{
    float t = u_temperature * 0.01;
    return c * vec3(1.0 + 0.10 * t, 1.0, 1.0 - 0.10 * t);
}

vec3 applyTint(vec3 c)
{
    float i = u_tint_intensity * 0.01;
    if (i <= 0.0) return c;
    vec3 hueRGB = hueToRgb(u_tint_color / 360.0);
    return mix(c, c * hueRGB, i * 0.5);
}

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

void main()
{
    vec4 src = texture(img, textureCoord);
    vec3 c   = src.rgb;

    c = applyTemperature(c);
    c = applyTint       (c);
    c = applyVibrance   (c);

    fragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
