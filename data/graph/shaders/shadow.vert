#version 130

// ArxModern shadow map pass: renders the distance to the light into one cube map face.
// u_transform == 0 : pre-transformed entity vertices, world position in a_worldPos
// u_transform == 1 : level geometry, world position in a_position

uniform mat4 u_lightViewProj;
uniform int u_transform;

in vec4 a_position;
in vec3 a_worldPos;
in vec2 a_texcoord0;
in float a_caster; // entity index of the vertex (entities only), -1 = none

out vec3 v_worldPos;
out vec2 v_texcoord0;
out float v_caster;

void main() {
	vec3 p = (u_transform == 0) ? a_worldPos : a_position.xyz;
	v_worldPos = p;
	v_texcoord0 = a_texcoord0;
	v_caster = a_caster;
	gl_Position = u_lightViewProj * vec4(p, 1.0);
}
