// ArxModern traced lighting (built with the "#version 430" / ARX_RT prelude, rt_common.glsl
// prepended): one pass at half resolution over the G-buffer of the scene, one ray per pixel and
// frame, accumulated over the frames (reprojected from the previous frame, reset where the
// depth no longer matches).
//
// Per pixel the ray leaves the surface in a random direction of its hemisphere (cosine
// weighted) and gives, at once:
// - the ambient occlusion: how close the ray hit something (replaces post_ssao.frag);
// - the indirect light: what the surface it hit looks like (its texture, its static light and
//   the dynamic lights reaching it, with a shadow ray each), so a wall lit by a torch lights
//   the room back and the coloured floors tint the walls;
// - with u_static, the shadows of the level's fixed lights: for each one reaching the pixel a
//   ray towards a point of the flame, and the ratio of what got through to what would have
//   without shadows. post_final.frag scales the static part of the lighting by it.
//
// Outputs (RGBA16, normalised): A = (indirect rgb / GiScale, occlusion), B = (static factor / 2,
// view depth / DepthScale, samples / MaxSamples, 1). post_trace_blur.frag smooths them, using
// the depth and the normals so that the blur stays on its surface.

#define MAX_LIGHTS 128

uniform sampler2D u_depth;    // scene depth, full size
uniform sampler2D u_normal;   // G-buffer: world normal * 0.5 + 0.5, a = 1 where something lit was drawn
uniform sampler2D u_historyA; // last frame's outputs (before the blur)
uniform sampler2D u_historyB;
uniform vec4 u_projection;    // (proj[0][0], proj[1][1], Q, Q * near): view z = Q * near / (Q - z_ndc)
uniform mat4 u_invView;       // view -> world
uniform mat4 u_prevViewProj;  // world -> clip of the previous frame (reprojection)
uniform vec3 u_cameraPos;
uniform ivec2 u_fullSize;     // size of the depth and normal textures
uniform int u_frame;          // frame counter (the sample pattern moves with it)
uniform int u_reset;          // 1: ignore the history
uniform int u_static;         // 1: also the shadows of the static lights
uniform float u_aoRadius;     // world units within which a hit occludes
uniform int u_lightCount;
uniform int u_dynamicLightCount; // the first lights are the dynamic ones (torches, spells), the rest the level's
uniform vec4 u_lightPos[MAX_LIGHTS];   // xyz, fallstart
uniform vec4 u_lightColor[MAX_LIGHTS]; // rgb, fallend

in vec2 v_uv;

layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;

// Tunables (a mod can edit this file: F7 reloads it)
const float GiScale = 4.0;        // indirect light stored as value / GiScale (post_final.frag undoes it)
const float DepthScale = 16384.0; // view depth stored as depth / DepthScale
const float MaxSamples = 64.0;    // frames accumulated at most (the static shadows converge to this)
const float GiSamples = 24.0;     // ... for the indirect light and the occlusion (a moving torch must follow)
const float GiRange = 1000.0;     // world units an indirect ray travels at most
const float HitBlur = 3.0;        // texture mip level read at the hit (128-pixel textures: 3 = 16 pixels)
const float LightScale = 0.5;     // the dynamic lights on level geometry (ApplyTileLights)
const float LightRadius = 12.0;   // world units: size of a light source (soft static shadows)
const float Ambient = 0.12;       // light assumed to reach everywhere (keeps the static factor from dropping to zero)
const float MinLight = 0.02;      // static lights contributing less than this are not traced
const float DepthTolerance = 0.03; // relative depth difference beyond which the history is dropped

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

// Interleaved gradient noise (Jimenez 2014)
float ign(vec2 p) {
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

float luma(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}

// What the level looks like at a hit point: its texture, its static light and the dynamic
// lights reaching it (a shadow ray each). Like reflect_rt.frag's shadeHit.
vec3 shadeHit(RtHit hit, vec3 point, vec3 dir) {
	uint tri = hit.tri;
	uint flags = rtFlags(tri);
	vec3 albedo = textureLod(u_rtTextures, vec3(rtUv(tri, hit.bary), float(rtLayer(tri))), HitBlur).rgb;
	if((flags & RtGlow) != 0u) {
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
		if(luma(u_lightColor[i].rgb) * cosangle * attenuation < MinLight) {
			continue;
		}
		if(!rtLit(u_lightPos[i].xyz, point + normal * 2.0)) {
			continue;
		}
		dynamic += u_lightColor[i].rgb * (cosangle * attenuation);
	}
	light = min(light + dynamic * LightScale, 1.0);
	return albedo * light;
}

void main() {

	// This half-resolution pixel stands for the full-resolution texel at twice its coordinates
	ivec2 full = min(ivec2(gl_FragCoord.xy) * 2, u_fullSize - 1);
	float zBuffer = texelFetch(u_depth, full, 0).r;
	vec4 gn = texelFetch(u_normal, full, 0);
	float z = linearDepth(zBuffer);

	if(gn.a < 0.5 || zBuffer >= 0.9999) {
		outA = vec4(0.0, 0.0, 0.0, 1.0);
		outB = vec4(0.5, z / DepthScale, 0.0, 1.0);
		return;
	}

	vec2 uvFull = (vec2(full) + 0.5) / vec2(u_fullSize);
	vec2 ndc = uvFull * 2.0 - 1.0;
	vec3 dirView = vec3(ndc.x / u_projection.x, ndc.y / u_projection.y, 1.0);
	vec3 world = u_cameraPos + (mat3(u_invView) * dirView) * z;
	vec3 normal = normalize(gn.xyz * 2.0 - 1.0);
	// Off the surface, more so far away (the depth loses precision there)
	vec3 origin = world + normal * (1.0 + z * 0.004);

	// Where the previous frame saw this surface: its accumulated values are carried over
	bool history = false;
	vec4 ha = vec4(0.0);
	vec4 hb = vec4(0.0);
	if(u_reset == 0) {
		vec4 prevClip = u_prevViewProj * vec4(world, 1.0);
		if(prevClip.w > 0.0) {
			vec2 prevUv = prevClip.xy / prevClip.w * 0.5 + 0.5;
			if(prevUv.x > 0.0 && prevUv.x < 1.0 && prevUv.y > 0.0 && prevUv.y < 1.0) {
				hb = texture(u_historyB, prevUv);
				float prevDepth = hb.g * DepthScale;
				if(abs(prevDepth - prevClip.w) < DepthTolerance * prevClip.w + 2.0) {
					ha = texture(u_historyA, prevUv);
					history = true;
				}
			}
		}
	}

	// Checkerboard: every other pixel is only traced every other frame, the history stands in
	// between (halves the cost; a pixel without history is traced anyway)
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	if(history && ((pixel.x + pixel.y + u_frame) & 1) == 1) {
		outA = ha;
		outB = vec4(hb.r, z / DepthScale, hb.b, 1.0);
		return;
	}

	// Two random numbers per pixel, moving with the frame (R2 sequence over the pixel noise)
	float n1 = ign(gl_FragCoord.xy);
	float n2 = ign(gl_FragCoord.yx + vec2(17.0, 31.0));
	float f = float(u_frame % 1024);
	float r1 = fract(n1 + f * 0.7548776662);
	float r2 = fract(n2 + f * 0.5698402910);

	// A cosine-weighted direction of the hemisphere
	vec3 up = (abs(normal.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tx = normalize(cross(up, normal));
	vec3 ty = cross(normal, tx);
	float phi = 6.2831853 * r1;
	float sinT = sqrt(r2);
	float cosT = sqrt(1.0 - r2);
	vec3 dir = tx * (cos(phi) * sinT) + ty * (sin(phi) * sinT) + normal * cosT;

	// Back faces are ignored (cullBack): the origin may sit a hair below its own floor
	float ao = 1.0;
	vec3 gi = vec3(0.0);
	RtHit hit;
	if(rtTrace(origin, dir, GiRange, false, true, hit)) {
		ao = smoothstep(0.0, u_aoRadius, hit.t);
		gi = shadeHit(hit, origin + dir * hit.t, dir);
	}

	// Static shadows: what reaches the pixel with the shadows over what would without them
	float factor = 1.0;
	if(u_static != 0) {
		float open = Ambient;
		float shadowed = Ambient;
		for(int i = u_dynamicLightCount; i < u_lightCount; i++) {
			vec3 toLight = u_lightPos[i].xyz - world;
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
			float c = luma(u_lightColor[i].rgb) * cosangle * attenuation;
			if(c < MinLight) {
				continue;
			}
			open += c;
			// One ray towards a random point of the flame, a different one per light and frame
			vec3 lup = (abs(toLight.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
			vec3 lx = normalize(cross(lup, toLight));
			vec3 ly = cross(toLight, lx);
			float angle = 6.2831853 * fract(r1 + float(i) * 0.618034);
			float radius = LightRadius * sqrt(fract(r2 + float(i) * 0.381966));
			if(rtLit(u_lightPos[i].xyz + (lx * cos(angle) + ly * sin(angle)) * radius, origin)) {
				shadowed += c;
			}
		}
		factor = shadowed / open;
	}

	// Blend with the history
	float samples = 0.0;
	vec3 giAcc = gi;
	float aoAcc = ao;
	float factorAcc = factor;
	if(history) {
		samples = hb.b * MaxSamples;
		float alphaGi = max(1.0 / (samples + 1.0), 1.0 / GiSamples);
		float alpha = 1.0 / (samples + 1.0);
		giAcc = mix(ha.rgb * GiScale, gi, alphaGi);
		aoAcc = mix(ha.a, ao, alphaGi);
		factorAcc = mix(hb.r * 2.0, factor, alpha);
	}
	samples = min(samples + 1.0, MaxSamples);

	outA = vec4(giAcc / GiScale, aoAcc);
	outB = vec4(factorAcc * 0.5, z / DepthScale, samples / MaxSamples, 1.0);
}
