// ArxModern SMAA pass 3: the composed image (u_scene) blended along its edges with the
// weights of pass 2 (u_blend). Written to the window.

uniform sampler2D u_scene;
uniform sampler2D u_blend;

in vec2 v_uv;
in vec4 v_offset;
out vec4 fragColor;

void main() {
	fragColor = SMAANeighborhoodBlendingPS(v_uv, v_offset, u_scene, u_blend);
}
