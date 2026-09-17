#version 130

// ArxModern water ripples: one step of a 2D wave equation on a height field that follows the
// camera (GLRipples.cpp). Red = height, green = vertical velocity, both stored normalised as
// 0.5 + x / Range in a 16-bit texture (a float format sampled as zeros in the water pass on
// the machine this was written on). Things moving in the water (a wading player, a falling
// object, an arrow) push the surface down at their position and the rings spread from there;
// the water shader (water.frag) reads the slopes of this map.

uniform sampler2D u_previous;
uniform vec2 u_shift;      // texels the window moved since the last step (the old state is read shifted)
uniform vec2 u_invSize;    // 1 / map size
uniform vec4 u_window;     // (origin x, origin z, size in world units, texel in world units)
uniform float u_speed;     // wave speed squared, in texels per step (< 0.5 for stability)
uniform float u_damping;   // velocity kept per step
uniform int u_sourceCount;
uniform vec4 u_sources[32]; // (x, z, radius, strength) in world units

in vec2 v_uv;

out vec4 fragColor;

const float Range = 64.0; // world units of height (or velocity) either way, see water.frag

float height(vec2 uv) {
	return (texture(u_previous, uv).r - 0.5) * Range;
}

void main() {

	vec2 uv = v_uv + u_shift * u_invSize;
	vec2 state = (texture(u_previous, uv).rg - 0.5) * Range;
	float h = state.r;
	float v = state.g;

	float laplacian = height(uv + vec2(-u_invSize.x, 0.0))
	                + height(uv + vec2(u_invSize.x, 0.0))
	                + height(uv + vec2(0.0, -u_invSize.y))
	                + height(uv + vec2(0.0, u_invSize.y))
	                - 4.0 * h;
	v = (v + laplacian * u_speed) * u_damping;
	h = (h + v) * 0.999; // (the 16-bit rounding would drift the whole map otherwise)

	vec2 world = u_window.xy + (v_uv - 0.5) * u_window.z;
	for(int i = 0; i < u_sourceCount; i++) {
		float d = distance(world, u_sources[i].xy);
		if(d < u_sources[i].z) {
			float k = 1.0 - d / u_sources[i].z;
			h -= u_sources[i].w * k * k;
		}
	}

	// The waves die out towards the edge of the window instead of bouncing on it
	vec2 e = min(v_uv, 1.0 - v_uv);
	float edge = smoothstep(0.0, 0.06, min(e.x, e.y));

	fragColor = vec4(0.5 + h * edge / Range, 0.5 + v * edge / Range, 0.0, 1.0);
}
