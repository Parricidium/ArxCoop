#version 130

// ArxModern "legacy" fragment shader: reproduces the fixed-function texture combiners
// (GL_COMBINE with GL_MODULATE / GL_REPLACE and RGB/alpha scale) and linear fog.
//
// Per stage, u_stageN = ivec2(colorOp, alphaOp) with the TextureStage::TextureOp values:
//   0 = disabled   : pass the previous stage through
//   1 = select     : texture
//   2 = modulate   : texture * previous
//   3 = modulate2x : texture * previous * 2
//   4 = modulate4x : texture * previous * 4
// A stage without a bound texture is reported as (0, 0).
//
// Per-pixel lighting, with the engine's own formula (scene/Light.cpp): Lambert term, constant
// up to fallstart, linear falloff to zero at fallend. u_lightPos = (xyz, fallstart),
// u_lightColor = (rgb, fallend), rgb premultiplied by intensity and the global light factor.
// - Level geometry (u_pixelLighting != 0): the vertex color carries the precomputed static
//   lighting and the first u_dynamicLightCount lights are added at half strength
//   (ApplyTileLights).
// - Entities (v_diffuse > 0): the vertex color carries the ambient term and all lights are
//   added, scaled by the diffuse factor (ApplyLight).

#define MAX_LIGHTS 128

uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_texture2;
uniform ivec2 u_stage0;
uniform ivec2 u_stage1;
uniform ivec2 u_stage2;

uniform int u_fogEnabled;
uniform vec3 u_fogColor;
uniform vec2 u_fogRange; // start, end

uniform int u_pixelLighting;
uniform int u_lightCount;
uniform int u_dynamicLightCount;
uniform vec4 u_lightPos[MAX_LIGHTS];
uniform vec4 u_lightColor[MAX_LIGHTS];

// Shadows: the first u_shadowCount dynamic lights have a cube map holding the distance
// from the light (normalised by fallend) of the nearest occluder in each direction.
uniform int u_shadowCount;
uniform samplerCube u_shadow0;
uniform samplerCube u_shadow1;
uniform samplerCube u_shadow2;
uniform samplerCube u_shadow3;

// Normal map of the current material (tangent space, texture unit 3), see u_normalMapped
uniform sampler2D u_normalMap;
uniform int u_normalMapped;
uniform float u_normalStrength;

in vec4 v_color;
in vec2 v_texcoord0;
in vec2 v_texcoord1;
in vec2 v_texcoord2;
in float v_fogDistance;
in vec3 v_worldPos;
in vec3 v_normal;
in float v_diffuse;

out vec4 fragColor;

vec3 combineColor(int op, vec3 tex, vec3 prev) {
	if(op == 1) {
		return tex;
	} else if(op == 2) {
		return min(tex * prev, 1.0);
	} else if(op == 3) {
		return min(tex * prev * 2.0, 1.0);
	} else if(op == 4) {
		return min(tex * prev * 4.0, 1.0);
	}
	return prev;
}

float combineAlpha(int op, float tex, float prev) {
	if(op == 1) {
		return tex;
	} else if(op == 2) {
		return min(tex * prev, 1.0);
	} else if(op == 3) {
		return min(tex * prev * 2.0, 1.0);
	} else if(op == 4) {
		return min(tex * prev * 4.0, 1.0);
	}
	return prev;
}

float shadowDistance(int i, vec3 dir) {
	if(i == 0) {
		return texture(u_shadow0, dir).r;
	} else if(i == 1) {
		return texture(u_shadow1, dir).r;
	} else if(i == 2) {
		return texture(u_shadow2, dir).r;
	}
	return texture(u_shadow3, dir).r;
}

// Fraction of the light reaching this fragment, with a small percentage-closer filter.
// fromLight = fragment - light position, dist = its length, fallend = light range,
// cosangle = cosine between the surface normal and the direction to the light.
float shadowFactor(int i, vec3 fromLight, float dist, float fallend, float cosangle) {
	const vec3 offsets[8] = vec3[8](
		vec3( 1.0,  1.0,  1.0), vec3( 1.0, -1.0,  1.0), vec3(-1.0, -1.0,  1.0), vec3(-1.0,  1.0,  1.0),
		vec3( 1.0,  1.0, -1.0), vec3( 1.0, -1.0, -1.0), vec3(-1.0, -1.0, -1.0), vec3(-1.0,  1.0, -1.0)
	);
	// Filter radius in world units, and the depth change it causes across a slanted surface
	float radius = dist * 0.006;
	float c = max(cosangle, 0.05);
	float slope = sqrt(1.0 - c * c) / c;
	float bias = (2.0 + radius * 1.8 * min(slope, 8.0)) / fallend;
	float current = dist / fallend;
	float lit = 0.0;
	for(int k = 0; k < 8; k++) {
		float stored = shadowDistance(i, fromLight + offsets[k] * radius);
		lit += (current - bias <= stored) ? 1.0 : 0.0;
	}
	return lit * (1.0 / 8.0);
}

// Perturb the interpolated normal with the material's normal map. The tangent frame is
// built from the screen-space derivatives of the world position and texture coordinates
// (cotangent frame, C. Schüler), so no per-vertex tangents are needed.
vec3 perturbedNormal(vec3 normal) {
	if(u_normalMapped == 0) {
		return normal;
	}
	vec3 dp1 = dFdx(v_worldPos);
	vec3 dp2 = dFdy(v_worldPos);
	vec2 duv1 = dFdx(v_texcoord0);
	vec2 duv2 = dFdy(v_texcoord0);
	vec3 dp2perp = cross(dp2, normal);
	vec3 dp1perp = cross(normal, dp1);
	vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
	if(invmax > 1e6) {
		return normal; // degenerate uv mapping
	}
	mat3 tbn = mat3(tangent * invmax, bitangent * invmax, normal);
	vec3 n = texture(u_normalMap, v_texcoord0).xyz * 2.0 - 1.0;
	n.xy *= u_normalStrength;
	return normalize(tbn * n);
}

void main() {

	vec4 color = v_color;

	int lightCount = 0;
	float lightScale = 0.0;
	if(u_pixelLighting != 0) {
		lightCount = u_dynamicLightCount;
		lightScale = 0.5;
	} else if(v_diffuse > 0.0) {
		lightCount = u_lightCount;
		lightScale = v_diffuse;
	}
	if(lightCount > 0) {
		vec3 normal = perturbedNormal(normalize(v_normal));
		vec3 light = vec3(0.0);
		for(int i = 0; i < lightCount; i++) {
			vec3 toLight = u_lightPos[i].xyz - v_worldPos;
			float dist = length(toLight);
			float fallend = u_lightColor[i].w;
			if(dist >= fallend) {
				continue;
			}
			float cosangle = dot(normal, toLight / dist);
			if(cosangle <= 0.0) {
				continue;
			}
			float fallstart = u_lightPos[i].w;
			float attenuation = (dist <= fallstart) ? 1.0 : (fallend - dist) / (fallend - fallstart);
			if(i < u_shadowCount) {
				// Offset along the normal so that a surface does not shadow itself
				vec3 fromLight = (v_worldPos + normal * (2.0 + dist * 0.01)) - u_lightPos[i].xyz;
				attenuation *= shadowFactor(i, fromLight, length(fromLight), fallend, cosangle);
				if(attenuation <= 0.0) {
					continue;
				}
			}
			light += u_lightColor[i].rgb * (cosangle * attenuation);
		}
		color.rgb = min(color.rgb + light * lightScale, 1.0);
	}

	if(u_stage0.x != 0 || u_stage0.y != 0) {
		vec4 tex = texture(u_texture0, v_texcoord0);
		color.rgb = combineColor(u_stage0.x, tex.rgb, color.rgb);
		color.a = combineAlpha(u_stage0.y, tex.a, color.a);
	}

	if(u_stage1.x != 0 || u_stage1.y != 0) {
		vec4 tex = texture(u_texture1, v_texcoord1);
		color.rgb = combineColor(u_stage1.x, tex.rgb, color.rgb);
		color.a = combineAlpha(u_stage1.y, tex.a, color.a);
	}

	if(u_stage2.x != 0 || u_stage2.y != 0) {
		vec4 tex = texture(u_texture2, v_texcoord2);
		color.rgb = combineColor(u_stage2.x, tex.rgb, color.rgb);
		color.a = combineAlpha(u_stage2.y, tex.a, color.a);
	}

	if(u_fogEnabled != 0) {
		// Linear fog, evaluated per fragment (the fixed-function result differs by a few
		// 8-bit steps in the distance, see README)
		float fog = clamp((u_fogRange.y - v_fogDistance) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		color.rgb = mix(u_fogColor, color.rgb, fog);
	}

	fragColor = color;

}
