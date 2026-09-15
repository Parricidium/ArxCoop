// ArxModern SMAA pass 1: luma edge detection of the composed image (u_scene).

uniform sampler2D u_scene;

in vec2 v_uv;
in vec4 v_offset[3];
out vec4 fragColor;

void main() {
	fragColor = vec4(SMAALumaEdgeDetectionPS(v_uv, v_offset, u_scene), 0.0, 0.0);
}
