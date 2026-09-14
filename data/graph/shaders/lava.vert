#version 130

// ArxModern lava: the lava polygons of the level (world space), drawn after the scene with
// the scene colour and depth available to the fragment shader. Drawn twice: the surface, and
// a "cap" raised by u_raise above the pool through which the heat haze is seen.

uniform mat4 u_viewProj;
uniform mat4 u_view;
uniform float u_raise;

in vec4 a_position;
in vec4 a_color; // r = 1 inside the pool, 0 at its edge (see RenderLava)
in vec2 a_texcoord0;
in vec2 a_texcoord1;
in vec2 a_texcoord2;

out vec3 v_worldPos;
out float v_viewDepth;
out float v_interior;
out vec2 v_uv0;
out vec2 v_uv1;
out vec2 v_uv2;

void main() {
	vec3 pos = a_position.xyz - vec3(0.0, u_raise, 0.0);
	v_worldPos = pos;
	v_viewDepth = abs((u_view * vec4(pos, 1.0)).z);
	v_interior = a_color.r;
	v_uv0 = a_texcoord0;
	v_uv1 = a_texcoord1;
	v_uv2 = a_texcoord2;
	gl_Position = u_viewProj * vec4(pos, 1.0);
}
