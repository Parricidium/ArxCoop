#version 130

// ArxModern screen-space reflections.
//
// The glossy level polygons (metal walls, marble and wet floors, ice, glass) are drawn again
// after the opaque scene: from each pixel a ray is reflected off the surface and marched
// through the scene depth buffer; where it hits, the scene colour there is what the surface
// reflects. What is off screen or hidden cannot be reflected: the effect fades out there.
// Blended over the scene with the reflection's own weight (Fresnel and glossiness).
//
// Inputs: u_scene / u_depth = the scene as rendered so far, u_texture0 = the material's
// texture (unit 0), u_normalMap = its material map (unit 3, see legacy.frag), u_material =
// (parallax, gloss scale, metalness, generated flag).

uniform sampler2D u_texture0;
uniform sampler2D u_normalMap;
uniform sampler2D u_scene;
uniform sampler2D u_depth;
uniform mat4 u_proj;
uniform mat4 u_view;
uniform vec4 u_projection; // (proj[0][0], proj[1][1], Q, Q * near): view z = Q * near / (Q - z_ndc)
uniform vec2 u_invSize;
uniform vec4 u_material;
uniform int u_normalMapped;
uniform float u_strength;  // 0..1, overall intensity
uniform int u_fogEnabled;
uniform vec2 u_fogRange;

in vec3 v_worldPos;
in vec3 v_viewPos;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const int Steps = 24;              // ray march steps
const float FirstStep = 6.0;       // world units
const float StepGrowth = 1.28;     // each step is this much longer than the previous
const float Thickness = 0.12;      // depth tolerance behind the surface hit, fraction of the distance
const float MinThickness = 12.0;   // ... at least this many units
const float MaxReflection = 0.85;  // weight of a perfect mirror at a grazing angle
const float MinReflection = 0.3;   // ... and head-on
const float RoughnessBend = 0.35;  // how much the material map bends the reflected ray

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

float sceneDepth(vec2 uv) {
	return linearDepth(texture(u_depth, uv).r);
}

// Screen position of a view-space point; w <= 0 when behind the camera
vec3 project(vec3 p) {
	vec4 clip = u_proj * vec4(p, 1.0);
	return vec3(clip.xy / clip.w * 0.5 + 0.5, clip.w);
}

// Cotangent frame from derivatives (see legacy.frag)
bool tangentFrame(vec3 normal, out mat3 tbn) {
	vec3 dp1 = dFdx(v_worldPos);
	vec3 dp2 = dFdy(v_worldPos);
	vec2 duv1 = dFdx(v_uv);
	vec2 duv2 = dFdy(v_uv);
	vec3 dp2perp = cross(dp2, normal);
	vec3 dp1perp = cross(normal, dp1);
	vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
	if(invmax > 1e6) {
		return false;
	}
	tbn = mat3(tangent * invmax, bitangent * invmax, normal);
	return true;
}

void main() {

	// Glossiness of this texel
	float gloss = u_material.y;
	vec3 normalWorld = normalize(v_normal);
	mat3 tbn;
	bool framed = (u_normalMapped != 0) && tangentFrame(normalWorld, tbn);
	if(framed) {
		vec4 material = texture(u_normalMap, v_uv);
		vec3 n;
		if(u_material.w != 0.0) {
			gloss *= material.a;
			n = vec3(material.rg * 2.0 - 1.0, 0.0);
			n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
		} else {
			n = material.xyz * 2.0 - 1.0;
		}
		// A rough surface reflects along a slightly bent normal (blurs the reflection)
		n.xy *= RoughnessBend * (1.0 - gloss);
		normalWorld = normalize(tbn * n);
	}
	if(gloss <= 0.02) {
		discard;
	}

	// Reflected ray in view space (camera at the origin, looking down +z)
	vec3 normal = normalize(mat3(u_view) * normalWorld);
	if(dot(normal, -v_viewPos) < 0.0) {
		normal = -normal; // the polygon is seen from behind
	}
	vec3 view = normalize(-v_viewPos);
	vec3 dir = reflect(-view, normal);

	// Fresnel, lifted head-on so that a glossy floor visibly mirrors (a wet look rather
	// than physically exact)
	float facing = max(dot(normal, view), 0.0);
	float fresnel = pow(1.0 - facing, 4.0);
	float weight = mix(mix(MinReflection, 1.0, fresnel), 1.0, u_material.z * 0.5) * gloss * MaxReflection * u_strength;
	// Rays towards the camera see what is behind it: nothing to show
	weight *= smoothstep(-0.15, 0.25, dir.z);
	if(weight <= 0.005) {
		discard;
	}

	// March
	vec3 p = v_viewPos;
	float step = FirstStep;
	float tPrev = 0.0, t = 0.0;
	bool hit = false;
	vec2 hitUv = vec2(0.0);
	for(int i = 0; i < Steps; i++) {
		tPrev = t;
		t += step;
		step *= StepGrowth;
		vec3 q = p + dir * t;
		vec3 s = project(q);
		if(s.z <= 0.0 || s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0) {
			break;
		}
		float sceneZ = sceneDepth(s.xy);
		float behind = q.z - sceneZ;
		if(behind > 0.0 && behind < max(sceneZ * Thickness, MinThickness)) {
			// Refine between the previous and this step
			float a = tPrev, b = t;
			for(int k = 0; k < 5; k++) {
				float m = 0.5 * (a + b);
				vec3 qm = p + dir * m;
				vec3 sm = project(qm);
				if(qm.z > sceneDepth(sm.xy)) {
					b = m;
				} else {
					a = m;
				}
			}
			hitUv = project(p + dir * b).xy;
			hit = true;
			break;
		}
	}
	if(!hit) {
		discard;
	}

	// Fade at the screen edges (the reflection would pop when the source leaves the view)
	vec2 edge = smoothstep(vec2(0.0), vec2(0.12), hitUv) * smoothstep(vec2(0.0), vec2(0.12), 1.0 - hitUv);
	weight *= edge.x * edge.y;

	vec3 reflected = texture(u_scene, hitUv).rgb;
	// Metals tint their reflections
	vec3 albedo = texture(u_texture0, v_uv).rgb;
	reflected *= mix(vec3(1.0), albedo * 1.5, u_material.z);

	if(u_fogEnabled != 0) {
		float fog = clamp((u_fogRange.y - v_viewPos.z) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		weight *= fog;
	}

	fragColor = vec4(reflected, weight);
}
