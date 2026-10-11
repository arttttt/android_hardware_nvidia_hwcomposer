#version 450
// Sample the source at the fragment, cut the corners by distance from the
// corner's centre, premultiply.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec2 size; float radius; } pc;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 p = gl_FragCoord.xy;
    vec4 c = texture(src, p / pc.size);
    vec2 q = abs(p - pc.size * 0.5) - (pc.size * 0.5 - vec2(pc.radius));
    float d = length(max(q, vec2(0.0))) - pc.radius;
    float a = clamp(0.5 - d, 0.0, 1.0);
    outColor = vec4(c.rgb * c.a * a, c.a * a);
}
