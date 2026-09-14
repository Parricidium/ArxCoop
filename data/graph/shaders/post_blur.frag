#version 130

// ArxModern bloom, pass 2: separable Gaussian blur; u_direction = (1/width, 0) or (0, 1/height).

uniform sampler2D u_source;
uniform vec2 u_direction;

in vec2 v_uv;
out vec4 fragColor;

void main() {
	const float weights[5] = float[5](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
	vec3 result = texture(u_source, v_uv).rgb * weights[0];
	for(int i = 1; i < 5; i++) {
		vec2 offset = u_direction * float(i);
		result += texture(u_source, v_uv + offset).rgb * weights[i];
		result += texture(u_source, v_uv - offset).rgb * weights[i];
	}
	fragColor = vec4(result, 1.0);
}
