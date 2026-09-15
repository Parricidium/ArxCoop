// ArxModern SMAA pass 1 (edge detection), vertex stage. The engine prepends the prelude
// (#version, SMAA_GLSL_3, preset, SMAA_RT_METRICS) and smaa.glsl to this file.

out vec2 v_uv;
out vec4 v_offset[3];

void main() {
	vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
	v_uv = p * 0.5 + 0.5;
	SMAAEdgeDetectionVS(v_uv, v_offset);
	gl_Position = vec4(p, 0.0, 1.0);
}
