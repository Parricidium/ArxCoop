// ArxModern SMAA pass 2 (blending weights), vertex stage.

out vec2 v_uv;
out vec2 v_pixcoord;
out vec4 v_offset[3];

void main() {
	vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
	v_uv = p * 0.5 + 0.5;
	SMAABlendingWeightCalculationVS(v_uv, v_pixcoord, v_offset);
	gl_Position = vec4(p, 0.0, 1.0);
}
