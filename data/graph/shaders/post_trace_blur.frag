#version 130

// ArxModern traced lighting, blur: one direction of a separable blur over the two outputs of
// post_trace.frag, kept on the surface by the depth stored in B.g and by the normals of the
// G-buffer (a wall does not bleed into the floor it meets). Run once horizontally and once
// vertically; the depth and sample count of the centre texel pass through untouched.

uniform sampler2D u_sourceA;
uniform sampler2D u_sourceB;
uniform sampler2D u_normal;   // G-buffer normal, full size
uniform vec2 u_direction;     // (1 / width, 0) or (0, 1 / height)

in vec2 v_uv;


// Tunables (a mod can edit this file: F7 reloads it)
const int Taps = 4;                 // taps either side
const float DepthSharpness = 40.0;  // weight = exp(-relative depth difference * this)
const float NormalPower = 8.0;      // weight *= dot(normals)^this

const float weights[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main() {

	vec4 centreA = texture(u_sourceA, v_uv);
	vec4 centreB = texture(u_sourceB, v_uv);
	float depth = centreB.g;
	vec3 normal = normalize(texture(u_normal, v_uv).xyz * 2.0 - 1.0);

	vec4 sumA = centreA * weights[0];
	float sumFactor = centreB.r * weights[0];
	float total = weights[0];
	for(int k = 1; k <= Taps; k++) {
		for(int s = -1; s <= 1; s += 2) {
			vec2 uv = v_uv + u_direction * float(k * s);
			vec4 b = texture(u_sourceB, uv);
			vec3 n = normalize(texture(u_normal, uv).xyz * 2.0 - 1.0);
			float w = weights[k] * exp(-abs(b.g - depth) / (depth + 1e-5) * DepthSharpness)
			        * pow(max(dot(n, normal), 0.0), NormalPower);
			sumA += texture(u_sourceA, uv) * w;
			sumFactor += b.r * w;
			total += w;
		}
	}

	gl_FragData[0] = sumA / total;
	gl_FragData[1] = vec4(sumFactor / total, centreB.g, centreB.b, 1.0);
}
