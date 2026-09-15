// ArxModern ray-traced reflections: the glossy level polygons (world space, SMY_VERTEX) drawn
// again after the opaque scene. Built with a "#version 430" prelude (see rt_common.glsl).

uniform mat4 u_viewProj;
uniform mat4 u_view;

in vec4 a_position;
in vec4 a_color;
in vec2 a_texcoord0;
in vec4 a_normal;

out vec3 v_worldPos;
out vec3 v_viewPos;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_color;

void main() {
	v_worldPos = a_position.xyz;
	v_viewPos = (u_view * vec4(a_position.xyz, 1.0)).xyz;
	v_normal = a_normal.xyz;
	v_uv = a_texcoord0;
	v_color = a_color;
	gl_Position = u_viewProj * vec4(a_position.xyz, 1.0);
}
