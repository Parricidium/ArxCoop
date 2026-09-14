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

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

float sceneDepth(vec2 uv) {
	return linearDepth(texture(u_depth, uv).r);
}

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
