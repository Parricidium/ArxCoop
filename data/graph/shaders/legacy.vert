#version 130

// ArxModern "legacy" vertex shader: reproduces the fixed-function transform.
//
// u_transform == 0 : vertices are pre-transformed (TexturedVertex, screen space with w),
//                    u_mvp maps pixels to clip space and the fog distance is the vertex w.
// u_transform == 1 : vertices are in world space, u_mvp = projection * view.

uniform mat4 u_mvp;
uniform mat4 u_view;
uniform int u_transform;

in vec4 a_position;
in vec4 a_color;
in vec2 a_texcoord0;
in vec2 a_texcoord1;
in vec2 a_texcoord2;
in vec4 a_normal;   // xyz = normal, w = diffuse factor (pre-transformed vertices only)
in vec3 a_worldPos; // pre-transformed vertices only

out vec4 v_color;
out vec2 v_texcoord0;
out vec2 v_texcoord1;
out vec2 v_texcoord2;
out float v_fogDistance;
out vec3 v_worldPos;
out vec3 v_normal;
out float v_diffuse;

void main() {

	gl_Position = u_mvp * a_position;

	v_color = a_color;
	v_texcoord0 = a_texcoord0;
	v_texcoord1 = a_texcoord1;
	v_texcoord2 = a_texcoord2;
	v_normal = a_normal.xyz;
	if(u_transform == 0) {
		v_worldPos = a_worldPos;
		v_diffuse = a_normal.w;
	} else {
		v_worldPos = a_position.xyz;
		v_diffuse = 0.0;
	}

	if(u_transform == 0) {
		// clip.w == view.z for pre-transformed vertices
		v_fogDistance = a_position.w;
	} else {
		// absolute eye-plane distance: the engine's view space looks down +z (D3D style),
		// and the fixed-function pipeline fogs with |z_eye| (GL_EYE_PLANE_ABSOLUTE_NV)
		v_fogDistance = abs((u_view * vec4(a_position.xyz, 1.0)).z);
	}

}
