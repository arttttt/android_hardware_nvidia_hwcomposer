#version 450
// The layer's colour, premultiplied, times its plane alpha and the
// rounded-corner mask; blended premultiplied-over by the pipeline.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec2 posTL;
    vec2 posTR;
    vec2 posBL;
    vec2 uvTL;
    vec2 uvTR;
    vec2 uvBL;
    vec4 color;
    vec4 corner;
    float radius;
    float alpha;
    int flags;
    float pad1;
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c;
    if ((pc.flags & 1) != 0) {
        c = vec4(pc.color.rgb * pc.color.a, pc.color.a);
    } else {
        c = texture(src, uv);
        if ((pc.flags & 4) != 0)
            c.a = 1.0;
        if ((pc.flags & 2) == 0)
            c.rgb *= c.a;
    }
    float m = pc.alpha;
    if (pc.radius > 0.0) {
        vec2 centre = (pc.corner.xy + pc.corner.zw) * 0.5;
        vec2 halfSize = (pc.corner.zw - pc.corner.xy) * 0.5;
        vec2 q = abs(gl_FragCoord.xy - centre) - (halfSize - vec2(pc.radius));
        // Signed distance to the rounded rectangle. The inner term is what
        // makes the inside negative all the way in: without it every pixel
        // inside sat at -radius, and a radius under half a pixel -- the end
        // of every recents animation -- faded the whole layer.
        float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0)
                  - pc.radius;
        m *= clamp(0.5 - d, 0.0, 1.0);
    }
    outColor = c * m;
}
