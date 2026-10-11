#version 450
void main() {
    float x = float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0;
    float y = float(gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
