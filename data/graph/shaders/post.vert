#version 130

// ArxModern post-processing: full-screen triangle generated from the vertex id, no attributes.

out vec2 v_uv;

void main() {
	vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
	v_uv = p * 0.5 + 0.5;
	gl_Position = vec4(p, 0.0, 1.0);
}
