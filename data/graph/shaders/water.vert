#version 130

// ArxModern water surface: the water polygons of the level (world space), drawn after the
// scene with the scene colour and depth available to the fragment shader.

uniform mat4 u_viewProj;
uniform mat4 u_view;

in vec4 a_position;
in vec4 a_color;
in vec2 a_texcoord0;
in vec2 a_texcoord1;
in vec2 a_texcoord2;

out vec3 v_worldPos;
out float v_viewDepth;
out vec2 v_uv0;
out vec2 v_uv1;
out vec2 v_uv2;

void main() {
	v_worldPos = a_position.xyz;
	v_viewDepth = abs((u_view * vec4(a_position.xyz, 1.0)).z);
	v_uv0 = a_texcoord0;
	v_uv1 = a_texcoord1;
	v_uv2 = a_texcoord2;
	gl_Position = u_viewProj * vec4(a_position.xyz, 1.0);
}
