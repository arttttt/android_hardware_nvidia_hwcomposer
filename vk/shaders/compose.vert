#version 450
// One layer: a quad, four vertices as a strip -- 0 top-left, 1 top-right,
// 2 bottom-left, 3 bottom-right. The texture coordinate is given at three
// corners and the fourth follows, the mapping being affine (vk/Geometry).
// The push-constant block is shared with compose.frag and mirrored by
// LayerConstants in vk/Compositor.cpp.
layout(push_constant) uniform PC {
    vec4 ndc;       // left, top, right, bottom in clip space
    vec2 uvTL;
    vec2 uvTR;
    vec2 uvBL;
    vec2 pad0;
    vec4 color;     // solid colour, straight alpha
    vec4 corner;    // rounded crop: left, top, right, bottom, target pixels
    float radius;   // 0: no corners
    float alpha;    // plane alpha
    int flags;      // 1 solid, 2 premultiplied, 4 opaque
    float pad1;
} pc;
layout(location = 0) out vec2 uv;
void main() {
    int i = gl_VertexIndex;
    float x = (i & 1) == 0 ? pc.ndc.x : pc.ndc.z;
    float y = (i & 2) == 0 ? pc.ndc.y : pc.ndc.w;
    uv = pc.uvTL + float(i & 1) * (pc.uvTR - pc.uvTL)
                 + float((i >> 1) & 1) * (pc.uvBL - pc.uvTL);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
