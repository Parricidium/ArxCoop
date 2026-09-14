#version 130

// ArxModern bloom, pass 1: keep what is brighter than the threshold (downsampled to half size).

uniform sampler2D u_scene;
uniform float u_threshold;

in vec2 v_uv;
out vec4 fragColor;

void main() {
	vec3 c = texture(u_scene, v_uv).rgb;
	float luma = dot(c, vec3(0.299, 0.587, 0.114));
	float amount = smoothstep(u_threshold, 1.0, luma);
	fragColor = vec4(c * amount, 1.0);
}
