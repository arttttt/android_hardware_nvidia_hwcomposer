#version 450
// One layer: a quad, four vertices as a strip -- 0 top-left, 1 top-right,
// 2 bottom-left, 3 bottom-right. The place and the texture coordinate are
// both given at three corners and the fourth follows, the mapping being
// affine (vk/Geometry): a rectangle, or a rectangle turned by any angle.
// The push-constant block is shared with compose.frag and mirrored by
// LayerConstants in vk/Compositor.cpp.
layout(push_constant) uniform PC {
    vec2 posTL;     // clip space
    vec2 posTR;
    vec2 posBL;
    vec2 uvTL;
    vec2 uvTR;
    vec2 uvBL;
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
    float across = float(i & 1);
    float down = float((i >> 1) & 1);
    uv = pc.uvTL + across * (pc.uvTR - pc.uvTL) + down * (pc.uvBL - pc.uvTL);
    vec2 pos = pc.posTL + across * (pc.posTR - pc.posTL) + down * (pc.posBL - pc.posTL);
    gl_Position = vec4(pos, 0.0, 1.0);
}
