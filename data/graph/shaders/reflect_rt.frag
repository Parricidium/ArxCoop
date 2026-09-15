// ArxModern ray-traced reflections (rt_common.glsl is prepended, "#version 430").
//
// Like reflect.frag, the glossy level polygons are drawn again after the opaque scene, but the
// reflected ray is traced through the level geometry itself instead of the depth buffer: what
// is off screen, behind the camera or hidden behind a pillar reflects too, and the reflection
// no longer fades at the screen edges. Where the point hit is visible on screen the scene
// colour there is used (exact: characters, particles, everything drawn); elsewhere the hit is
// shaded from the level textures, its static light and the dynamic lights (with a shadow ray
// each, so the reflected torches cast shadows).
//
// u_mode 1 is a debugging view (post_debug=rt): every level polygon shows what a ray from the
// camera hits there - it must look like the scene itself, minus the characters.

#define MAX_RT_LIGHTS 32

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
uniform vec3 u_fogColor;
uniform vec2 u_fogRange;
uniform vec3 u_cameraPos;
uniform int u_mode;
uniform int u_shadowRays;  // shadow rays towards the dynamic lights at the hit point
uniform int u_lightCount;  // dynamic lights (torches, spells), like legacy.frag
uniform vec4 u_lightPos[MAX_RT_LIGHTS];   // xyz, fallstart
uniform vec4 u_lightColor[MAX_RT_LIGHTS]; // rgb, fallend

in vec3 v_worldPos;
in vec3 v_viewPos;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const float MaxDistance = 8000.0;  // world units a reflected ray travels at most
const float MaxReflection = 0.85;  // weight of a perfect mirror at a grazing angle
const float MinReflection = 0.3;   // ... and head-on
const float RoughnessBend = 0.35;  // how much the material map bends the reflected ray
const float RoughnessBlur = 3.0;   // texture mip levels of blur on the hit at gloss 0
const float SceneTolerance = 0.03; // depth match (fraction) for reusing the on-screen colour
const float LightScale = 0.5;      // the dynamic lights on level geometry (ApplyTileLights)

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
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

// What the level looks like at a hit point, seen along dir (blur = texture mip level)
vec3 shadeHit(RtHit hit, vec3 point, vec3 dir, float blur) {
	uint tri = hit.tri;
	uint flags = rtFlags(tri);
	vec3 albedo = textureLod(u_rtTextures, vec3(rtUv(tri, hit.bary), float(rtLayer(tri))), blur).rgb;
	if((flags & RtGlow) != 0u) {
		return albedo;
	}
	vec3 light = rtColor(tri, hit.bary);
	vec3 normal = normalize(rtNormal(tri));
	if(dot(normal, dir) > 0.0) {
		normal = -normal; // seen from behind
	}
	vec3 dynamic = vec3(0.0);
	for(int i = 0; i < u_lightCount; i++) {
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
		if(u_shadowRays != 0 && !rtLit(u_lightPos[i].xyz, point + normal * 2.0)) {
			continue;
		}
		dynamic += u_lightColor[i].rgb * (cosangle * attenuation);
	}
	light = min(light + dynamic * LightScale, 1.0);
	return albedo * light;
}

void main() {

	if(u_mode == 1) {
		// Debug: primary rays
		vec3 dir = normalize(v_worldPos - u_cameraPos);
		RtHit hit;
		if(!rtTrace(u_cameraPos, dir, MaxDistance, false, false, hit)) {
			fragColor = vec4(1.0, 0.0, 1.0, 1.0);
			return;
		}
		vec3 point = u_cameraPos + dir * hit.t;
		vec3 color = shadeHit(hit, point, dir, 0.0);
		if(u_fogEnabled != 0) {
			float fog = clamp((u_fogRange.y - hit.t) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
			color = mix(u_fogColor, color, fog);
		}
		fragColor = vec4(color, 1.0);
		return;
	}

	// Glossiness of this texel
	float gloss = u_material.y;
	vec3 geometric = normalize(v_normal);
	vec3 normal = geometric;
	mat3 tbn;
	bool framed = (u_normalMapped != 0) && tangentFrame(geometric, tbn);
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
		normal = normalize(tbn * n);
	}
	if(gloss <= 0.02) {
		discard;
	}

	vec3 toCamera = u_cameraPos - v_worldPos;
	vec3 view = normalize(toCamera);
	if(dot(geometric, view) < 0.0) {
		geometric = -geometric; // the polygon is seen from behind
		normal = -normal;
	}
	vec3 dir = reflect(-view, normal);
	if(dot(dir, geometric) < 0.02) {
		dir = normalize(dir - geometric * (dot(dir, geometric) - 0.02)); // keep it off the surface
	}

	// Fresnel, lifted head-on so that a glossy floor visibly mirrors (a wet look rather
	// than physically exact)
	float facing = max(dot(normal, view), 0.0);
	float fresnel = pow(1.0 - facing, 4.0);
	float weight = mix(mix(MinReflection, 1.0, fresnel), 1.0, u_material.z * 0.5) * gloss * MaxReflection * u_strength;
	if(weight <= 0.005) {
		discard;
	}

	// Trace
	vec3 origin = v_worldPos + geometric * 1.0;
	RtHit hit;
	if(!rtTrace(origin, dir, MaxDistance, false, false, hit)) {
		discard;
	}
	vec3 point = origin + dir * hit.t;

	// On screen and not hidden: take the scene colour there (characters, particles included)
	vec3 reflected;
	vec4 clip = u_proj * (u_view * vec4(point, 1.0));
	vec3 viewPoint = (u_view * vec4(point, 1.0)).xyz;
	bool onScreen = false;
	if(clip.w > 0.0) {
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		if(uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
			float sceneZ = linearDepth(texture(u_depth, uv).r);
			if(abs(sceneZ - viewPoint.z) < max(viewPoint.z * SceneTolerance, 8.0)) {
				reflected = texture(u_scene, uv).rgb;
				onScreen = true;
			}
		}
	}
	if(!onScreen) {
		float blur = (1.0 - gloss) * RoughnessBlur + clamp(log2(hit.t / 512.0), 0.0, 3.0);
		reflected = shadeHit(hit, point, dir, blur);
		if(u_fogEnabled != 0) {
			// The light travels from the hit to the surface and on to the camera
			float travelled = length(toCamera) + hit.t;
			float fog = clamp((u_fogRange.y - travelled) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
			reflected = mix(u_fogColor, reflected, fog);
		}
	}

	// Metals tint their reflections
	vec3 albedo = texture(u_texture0, v_uv).rgb;
	reflected *= mix(vec3(1.0), albedo * 1.5, u_material.z);

	if(u_fogEnabled != 0) {
		float fog = clamp((u_fogRange.y - v_viewPos.z) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		weight *= fog;
	}

	fragColor = vec4(reflected, weight);
}
