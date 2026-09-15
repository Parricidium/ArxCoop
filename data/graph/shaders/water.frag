#version 130

// ArxModern water surface.
//
// The engine draws water as a flat polygon with its own texture (already in the scene, with
// the floor below it showing through) and used to add three scrolling copies of the "enviro"
// highlight texture on top. This pass replaces that overlay: the scene behind the surface is
// read back through animated waves (refraction), the torches and other lights leave specular
// trails on the ripples, the water gets a little darker with depth, and the surface fades out
// against the banks instead of ending in a hard line.
//
// Inputs from the engine: u_scene / u_depth = the scene as rendered so far (resolved copies),
// u_enviro = the original highlight texture (unit 0) with its three scrolling uv sets,
// u_lightPos = (xyz, fallstart), u_lightColor = (rgb * intensity, fallend) as in legacy.frag.
//
// Reflections (u_reflection > 0): the scene mirrored in the surface, stronger at grazing angles.
// The reflected ray is first marched through the depth buffer (what is on screen: characters,
// the walls in view); with the ray tracing on (ARX_RT, rt_common.glsl prepended) whatever that
// misses is traced through the level geometry instead, so the ceiling and the walls behind the
// camera reflect too.

#define MAX_LIGHTS 128

uniform sampler2D u_enviro;
uniform sampler2D u_scene;
uniform sampler2D u_depth;
uniform vec2 u_invSize;
uniform vec4 u_projection; // (proj[0][0], proj[1][1], Q, Q * near): view z = Q * near / (Q - z_ndc)
uniform vec3 u_cameraPos;
uniform float u_time;
uniform float u_strength; // 0..1, overall intensity of the effect
uniform int u_fogEnabled;
uniform vec2 u_fogRange;
uniform int u_lightCount;
uniform vec4 u_lightPos[MAX_LIGHTS];
uniform vec4 u_lightColor[MAX_LIGHTS];
uniform int u_dynamicLightCount; // the first lights are the dynamic ones (torches, spells)
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_reflection; // 0..1, strength of the mirrored scene (0 = off)
uniform vec3 u_fogColor;

in vec3 v_worldPos;
in float v_viewDepth;
in vec2 v_uv0;
in vec2 v_uv1;
in vec2 v_uv2;

out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const float WaveHeight = 1.0;        // relative amplitude of the procedural waves
const float RippleDetail = 0.35;     // how much the enviro texture perturbs the normal
const float Refraction = 0.035;      // screen-space offset at full depth
const float RefractionDepth = 40.0;  // world units of water depth for full refraction
const float ShoreFade = 12.0;        // world units over which the surface fades at the banks
const float TintDepth = 250.0;       // world units of depth for full tint
const vec3 TintColor = vec3(0.55, 0.72, 0.85);
const float Specular = 1.2;
const float Shininess = 160.0;
const float LegacyMix = 0.2;         // how much of the original highlight overlay is kept
const float ReflectMin = 0.1;        // weight of the mirrored scene head-on...
const float ReflectMax = 0.8;        // ... and at a grazing angle
const float ReflectBend = 0.5;       // how much the waves bend the reflected ray (1 = fully)
const int Steps = 24;                // screen-space march steps
const float FirstStep = 6.0;
const float StepGrowth = 1.28;
const float Thickness = 0.12;
const float MinThickness = 12.0;
const float MaxDistance = 8000.0;    // world units a traced ray travels at most

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

float sceneDepth(vec2 uv) {
	return linearDepth(texture(u_depth, uv).r);
}

// Screen position of a view-space point; z <= 0 when behind the camera
vec3 project(vec3 p) {
	vec4 clip = u_proj * vec4(p, 1.0);
	return vec3(clip.xy / clip.w * 0.5 + 0.5, clip.w);
}

// March the reflected ray through the depth buffer (see reflect.frag); false when it leaves
// the screen or finds nothing
bool marchScreen(vec3 worldPos, vec3 worldDir, out vec3 color) {
	vec3 p = (u_view * vec4(worldPos, 1.0)).xyz;
	vec3 dir = mat3(u_view) * worldDir;
	if(dir.z < -0.15) {
		return false; // towards the camera: what it would show is behind us
	}
	float step = FirstStep;
	float tPrev = 0.0, t = 0.0;
	for(int i = 0; i < Steps; i++) {
		tPrev = t;
		t += step;
		step *= StepGrowth;
		vec3 q = p + dir * t;
		vec3 s = project(q);
		if(s.z <= 0.0 || s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0) {
			return false;
		}
		float sceneZ = sceneDepth(s.xy);
		float behind = q.z - sceneZ;
		if(behind > 0.0 && behind < max(sceneZ * Thickness, MinThickness)) {
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
			vec2 hitUv = project(p + dir * b).xy;
			// Fade at the screen edges (the traced ray takes over there when available)
			vec2 edge = smoothstep(vec2(0.0), vec2(0.08), hitUv) * smoothstep(vec2(0.0), vec2(0.08), 1.0 - hitUv);
			if(edge.x * edge.y < 0.5) {
				return false;
			}
			color = texture(u_scene, hitUv).rgb;
			return true;
		}
	}
	return false;
}

#ifdef ARX_RT
// What the level looks like at a traced hit (see reflect_rt.frag)
vec3 shadeHit(RtHit hit, vec3 point, vec3 dir, float blur) {
	uint tri = hit.tri;
	vec3 albedo = textureLod(u_rtTextures, vec3(rtUv(tri, hit.bary), float(rtLayer(tri))), blur).rgb;
	if((rtFlags(tri) & RtGlow) != 0u) {
		return albedo;
	}
	vec3 light = rtColor(tri, hit.bary);
	vec3 normal = normalize(rtNormal(tri));
	if(dot(normal, dir) > 0.0) {
		normal = -normal;
	}
	vec3 dynamic = vec3(0.0);
	for(int i = 0; i < u_dynamicLightCount; i++) {
		vec3 toLight = u_lightPos[i].xyz - point;
		float dist = length(toLight);
		float fallend = u_lightColor[i].w;
		if(dist >= fallend) {
			continue;
		}
		toLight /= dist;
		float cosangle = dot(normal, toLight);
		if(cosangle <= 0.0) {
			continue;
		}
		float fallstart = u_lightPos[i].w;
		float attenuation = (dist <= fallstart) ? 1.0 : (fallend - dist) / (fallend - fallstart);
		if(!rtLit(u_lightPos[i].xyz, point + normal * 2.0)) {
			continue;
		}
		dynamic += u_lightColor[i].rgb * (cosangle * attenuation);
	}
	return albedo * min(light + dynamic * 0.5, 1.0);
}

// Trace the reflected ray through the level; the scene colour where the hit is on screen
bool traceLevel(vec3 origin, vec3 dir, float pathSoFar, out vec3 color) {
	RtHit hit;
	if(!rtTrace(origin, dir, MaxDistance, false, false, hit)) {
		return false;
	}
	vec3 point = origin + dir * hit.t;
	vec3 viewPoint = (u_view * vec4(point, 1.0)).xyz;
	vec3 s = project(viewPoint);
	if(s.z > 0.0 && s.x >= 0.0 && s.x <= 1.0 && s.y >= 0.0 && s.y <= 1.0
	   && abs(sceneDepth(s.xy) - viewPoint.z) < max(viewPoint.z * 0.03, 8.0)) {
		color = texture(u_scene, s.xy).rgb;
		return true;
	}
	color = shadeHit(hit, point, dir, clamp(log2(hit.t / 512.0), 0.0, 3.0) + 1.0);
	if(u_fogEnabled != 0) {
		float fog = clamp((u_fogRange.y - (pathSoFar + hit.t)) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		color = mix(u_fogColor, color, fog);
	}
	return true;
}
#endif

// Sum of three plane waves: height and its slopes along two tangent directions
void waves(vec2 p, out float dhdu, out float dhdv) {
	dhdu = 0.0;
	dhdv = 0.0;
	// (direction, wavelength, speed, amplitude)
	vec2 d0 = normalize(vec2(1.0, 0.3));
	vec2 d1 = normalize(vec2(-0.4, 1.0));
	vec2 d2 = normalize(vec2(0.7, -0.8));
	float k0 = 6.2832 / 140.0, k1 = 6.2832 / 75.0, k2 = 6.2832 / 32.0;
	float a0 = 1.6, a1 = 0.9, a2 = 0.35;
	float ph0 = dot(d0, p) * k0 + u_time * 0.9;
	float ph1 = dot(d1, p) * k1 + u_time * 1.4;
	float ph2 = dot(d2, p) * k2 + u_time * 2.3;
	dhdu += a0 * k0 * cos(ph0) * d0.x + a1 * k1 * cos(ph1) * d1.x + a2 * k2 * cos(ph2) * d2.x;
	dhdv += a0 * k0 * cos(ph0) * d0.y + a1 * k1 * cos(ph1) * d1.y + a2 * k2 * cos(ph2) * d2.y;
	dhdu *= WaveHeight;
	dhdv *= WaveHeight;
}

void main() {

	// Geometric normal of the polygon (flat pools, but also waterfalls), facing the camera
	vec3 dpdx = dFdx(v_worldPos);
	vec3 dpdy = dFdy(v_worldPos);
	vec3 geoNormal = normalize(cross(dpdx, dpdy));
	vec3 toCamera = u_cameraPos - v_worldPos;
	float toCameraLength = length(toCamera);
	vec3 view = toCamera / max(toCameraLength, 0.001);
	if(dot(geoNormal, view) < 0.0) {
		geoNormal = -geoNormal;
	}

	// Tangent frame on the surface and the perturbed normal
	vec3 tangent = normalize(abs(geoNormal.y) > 0.9 ? cross(geoNormal, vec3(1.0, 0.0, 0.0)) : cross(geoNormal, vec3(0.0, 1.0, 0.0)));
	vec3 bitangent = cross(geoNormal, tangent);
	vec2 surfacePos = vec2(dot(v_worldPos, tangent), dot(v_worldPos, bitangent));
	float dhdu, dhdv;
	waves(surfacePos, dhdu, dhdv);
	vec2 ripple = (texture(u_enviro, v_uv1).rg - texture(u_enviro, v_uv2).gb) * RippleDetail;
	vec3 normal = normalize(geoNormal - tangent * (dhdu * u_strength + ripple.x) - bitangent * (dhdv * u_strength + ripple.y));

	// Scene behind the surface, refracted where the water is deep enough
	vec2 uv = gl_FragCoord.xy * u_invSize;
	float surfaceZ = linearDepth(gl_FragCoord.z);
	float thickness = max(sceneDepth(uv) - surfaceZ, 0.0);
	vec2 offsetDir = vec2(dot(normal - geoNormal, tangent), dot(normal - geoNormal, bitangent));
	vec2 offset = offsetDir * Refraction * u_strength * clamp(thickness / RefractionDepth, 0.0, 1.0);
	vec2 uv2 = clamp(uv + offset, u_invSize, 1.0 - u_invSize);
	float sceneZ2 = sceneDepth(uv2);
	if(sceneZ2 < surfaceZ) {
		// Something stands between the camera and the water there: no refraction across it
		uv2 = uv;
		sceneZ2 = sceneDepth(uv);
	}
	vec3 scene = texture(u_scene, uv2).rgb;
	float depth2 = max(sceneZ2 - surfaceZ, 0.0);

	// The original overlay, toned down: three scrolling highlight layers modulated together
	vec3 overlay = texture(u_enviro, v_uv0).rgb * texture(u_enviro, v_uv1).rgb * 4.0 * texture(u_enviro, v_uv2).rgb * 0.314;
	vec3 color = scene * (1.0 + overlay * LegacyMix) + overlay * LegacyMix * 0.25;

	// A little darker and bluer with depth
	float tint = clamp(depth2 / TintDepth, 0.0, 1.0) * u_strength;
	color *= mix(vec3(1.0), TintColor, tint);

	// Fresnel: glossier at grazing angles
	float facing = max(dot(normal, view), 0.0);
	float fresnel = 0.03 + 0.97 * pow(1.0 - facing, 5.0);

	// The mirrored scene: along the reflected ray, bent a little by the waves
	if(u_reflection > 0.0) {
		vec3 mirrorNormal = normalize(mix(geoNormal, normal, ReflectBend));
		vec3 dir = reflect(-view, mirrorNormal);
		if(dot(dir, geoNormal) < 0.05) {
			dir = normalize(dir - geoNormal * (dot(dir, geoNormal) - 0.05)); // keep it off the surface
		}
		vec3 mirrored;
		bool found = marchScreen(v_worldPos + geoNormal * 0.5, dir, mirrored);
#ifdef ARX_RT
		if(!found) {
			found = traceLevel(v_worldPos + geoNormal * 1.0, dir, toCameraLength, mirrored);
		}
#endif
		if(found) {
			float weight = mix(ReflectMin, ReflectMax, pow(1.0 - max(dot(geoNormal, view), 0.0), 3.0)) * u_reflection;
			color = mix(color, mirrored, weight);
		}
	}

	// Specular trails of the scene's lights (same attenuation as the engine)
	vec3 specular = vec3(0.0);
	for(int i = 0; i < u_lightCount; i++) {
		vec3 toLight = u_lightPos[i].xyz - v_worldPos;
		float dist = length(toLight);
		float fallend = u_lightColor[i].w;
		if(dist >= fallend) {
			continue;
		}
		float fallstart = u_lightPos[i].w;
		float attenuation = (dist <= fallstart) ? 1.0 : (fallend - dist) / (fallend - fallstart);
		vec3 halfway = normalize(toLight / dist + view);
		float s = pow(max(dot(normal, halfway), 0.0), Shininess);
		specular += u_lightColor[i].rgb * (s * attenuation);
	}
	color += specular * (Specular * u_strength * (0.4 + 0.6 * fresnel));

	// Fog dims the highlights as it does the scene
	if(u_fogEnabled != 0) {
		float fog = clamp((u_fogRange.y - v_viewDepth) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		color = mix(scene, color, fog);
	}

	// Fade against the banks and anything standing in the water
	float shore = clamp(thickness / ShoreFade, 0.0, 1.0);
	color = mix(texture(u_scene, uv).rgb, color, shore);

	fragColor = vec4(color, 1.0);
}
