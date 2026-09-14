#version 130

// ArxModern screen-space ambient occlusion, computed at half resolution from the scene depth.
//
// The engine's projection (game/Camera.cpp) is D3D-style: z_clip = Q * z - Q * near, w_clip = z,
// so the linear view depth is  z = Q * near / (Q - z_ndc).
// u_projection = (proj[0][0], proj[1][1], Q, Q * near)

uniform sampler2D u_depth;
uniform vec4 u_projection;
uniform vec2 u_invSize;   // 1 / depth texture size
uniform float u_radius;   // world units
uniform float u_bias;     // world units

in vec2 v_uv;
out vec4 fragColor;

float linearDepth(vec2 uv) {
	float d = texture(u_depth, uv).r;
	float zNdc = d * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

vec3 viewPosition(vec2 uv) {
	float z = linearDepth(uv);
	vec2 ndc = uv * 2.0 - 1.0;
	return vec3(ndc.x * z / u_projection.x, ndc.y * z / u_projection.y, z);
}

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {

	vec3 position = viewPosition(v_uv);
	if(position.z >= 0.98 * (u_projection.w / (u_projection.z - 1.0))) {
		// far plane (background): no occlusion
		fragColor = vec4(1.0);
		return;
	}

	// Normal from the depth derivatives, picking the neighbour on each axis that is closest
	// in depth so that depth discontinuities (silhouettes) do not produce wrong normals
	vec3 right = viewPosition(v_uv + vec2(u_invSize.x, 0.0));
	vec3 left = viewPosition(v_uv - vec2(u_invSize.x, 0.0));
	vec3 up = viewPosition(v_uv + vec2(0.0, u_invSize.y));
	vec3 down = viewPosition(v_uv - vec2(0.0, u_invSize.y));
	vec3 dx = (abs(right.z - position.z) < abs(position.z - left.z)) ? (right - position) : (position - left);
	vec3 dy = (abs(up.z - position.z) < abs(position.z - down.z)) ? (up - position) : (position - down);
	vec3 normal = normalize(cross(dx, dy));
	if(dot(normal, -position) < 0.0) {
		normal = -normal;
	}

	// Random rotation around the normal
	float angle = hash(gl_FragCoord.xy) * 6.2831853;
	vec3 randomVec = vec3(cos(angle), sin(angle), 0.0);
	vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
	vec3 bitangent = cross(normal, tangent);

	const int SAMPLES = 12;
	const vec3 kernel[12] = vec3[12](
		vec3( 0.2024,  0.8412, 0.4986), vec3(-0.2201, -0.1682, 0.5311), vec3( 0.1226,  0.0455, 0.1096),
		vec3(-0.0621,  0.2264, 0.2044), vec3( 0.6421, -0.0826, 0.4063), vec3(-0.3846, -0.4783, 0.3162),
		vec3( 0.0392, -0.1149, 0.0894), vec3(-0.7148,  0.2186, 0.3517), vec3( 0.2937,  0.3341, 0.1802),
		vec3(-0.1108, -0.0361, 0.0640), vec3( 0.5011, -0.6096, 0.2270), vec3(-0.3212,  0.0855, 0.0985)
	);

	float occlusion = 0.0;
	for(int i = 0; i < SAMPLES; i++) {
		vec3 k = kernel[i];
		vec3 samplePos = position + (tangent * k.x + bitangent * k.y + normal * k.z) * u_radius;
		if(samplePos.z <= 0.0) {
			continue;
		}
		// Project back to screen
		vec2 sampleUv = vec2(samplePos.x * u_projection.x / samplePos.z, samplePos.y * u_projection.y / samplePos.z) * 0.5 + 0.5;
		if(sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) {
			continue;
		}
		float sceneZ = linearDepth(sampleUv);
		float rangeCheck = smoothstep(0.0, 1.0, u_radius / abs(position.z - sceneZ));
		// Depth precision drops with distance: grow the bias with it
		float bias = u_bias + position.z * 0.004;
		occlusion += ((sceneZ <= samplePos.z - bias) ? 1.0 : 0.0) * rangeCheck;
	}

	float ao = 1.0 - occlusion / float(SAMPLES);
	fragColor = vec4(ao, ao, ao, 1.0);
}
