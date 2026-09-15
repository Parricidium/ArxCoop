// ArxModern SMAA pass 2: blending weights from the edges (u_edges) and the precomputed
// area (u_area) and search (u_search) textures.

uniform sampler2D u_edges;
uniform sampler2D u_area;
uniform sampler2D u_search;

in vec2 v_uv;
in vec2 v_pixcoord;
in vec4 v_offset[3];
out vec4 fragColor;

void main() {
	fragColor = SMAABlendingWeightCalculationPS(v_uv, v_pixcoord, v_offset, u_edges, u_area, u_search, vec4(0.0));
}
