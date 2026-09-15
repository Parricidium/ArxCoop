#version 130

// ArxModern post-processing, final pass: scene + bloom, then FXAA. Written to the window.
//
// u_bloom      : bloom intensity (0 disables)
// u_ao         : ambient occlusion strength (0 disables)
// u_darkness   : 0..1, how much the dark places are pushed towards black (the "Ambiance" option:
//                what the static lighting left dim becomes really dark, only the torches, spells
//                and lit areas keep their brightness)
// u_fxaa       : 1 to enable the anti-aliasing filter
// u_invSize    : 1 / window size in pixels

uniform sampler2D u_scene;
uniform sampler2D u_bloomTexture;
uniform sampler2D u_aoTexture;
uniform sampler2D u_volumeTexture; // rgb = in-scattered light, a = transmittance (post_volume.frag)
uniform int u_volumetric;
uniform float u_bloom;
uniform float u_ao;       // ambient occlusion strength (0 disables)
uniform float u_darkness;
uniform int u_fxaa;
uniform vec2 u_invSize;
uniform int u_debug;      // 1 = show the ambient occlusion buffer, 2 = the bloom buffer

in vec2 v_uv;
out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const float DarkKnee = 0.4; // luma below which the darkness curve bites (above: untouched)

float luma(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}

vec3 sceneAt(vec2 uv) {
	vec3 c = texture(u_scene, uv).rgb;
	if(u_ao > 0.0) {
		c *= mix(1.0, texture(u_aoTexture, uv).r, u_ao);
	}
	if(u_volumetric != 0) {
		vec4 haze = texture(u_volumeTexture, uv);
		c = c * haze.a + haze.rgb;
	}
	if(u_darkness > 0.0) {
		// Smooth toe: the darker a pixel already is, the more it is pulled towards black
		float l = luma(c);
		c *= mix(1.0, smoothstep(0.0, DarkKnee, l), u_darkness);
	}
	if(u_bloom > 0.0) {
		c += texture(u_bloomTexture, uv).rgb * u_bloom;
	}
	return c;
}

// Compact FXAA (after Timothy Lottes' FXAA 3.11, console/quality style)
vec3 fxaa(vec2 uv) {
	const float EDGE_THRESHOLD_MIN = 0.0312;
	const float EDGE_THRESHOLD_MAX = 0.125;
	const float SUBPIXEL_QUALITY = 0.75;
	const int ITERATIONS = 8;

	vec3 colorCenter = sceneAt(uv);
	float lumaCenter = luma(colorCenter);
	float lumaDown  = luma(sceneAt(uv + vec2(0.0, -u_invSize.y)));
	float lumaUp    = luma(sceneAt(uv + vec2(0.0,  u_invSize.y)));
	float lumaLeft  = luma(sceneAt(uv + vec2(-u_invSize.x, 0.0)));
	float lumaRight = luma(sceneAt(uv + vec2( u_invSize.x, 0.0)));

	float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
	float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
	float lumaRange = lumaMax - lumaMin;
	if(lumaRange < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX)) {
		return colorCenter;
	}

	float lumaDownLeft  = luma(sceneAt(uv + vec2(-u_invSize.x, -u_invSize.y)));
	float lumaUpRight   = luma(sceneAt(uv + vec2( u_invSize.x,  u_invSize.y)));
	float lumaUpLeft    = luma(sceneAt(uv + vec2(-u_invSize.x,  u_invSize.y)));
	float lumaDownRight = luma(sceneAt(uv + vec2( u_invSize.x, -u_invSize.y)));

	float lumaDownUp = lumaDown + lumaUp;
	float lumaLeftRight = lumaLeft + lumaRight;
	float lumaLeftCorners = lumaDownLeft + lumaUpLeft;
	float lumaDownCorners = lumaDownLeft + lumaDownRight;
	float lumaRightCorners = lumaDownRight + lumaUpRight;
	float lumaUpCorners = lumaUpRight + lumaUpLeft;

	float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0
	                     + abs(-2.0 * lumaRight + lumaRightCorners);
	float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0
	                   + abs(-2.0 * lumaDown + lumaDownCorners);
	bool isHorizontal = (edgeHorizontal >= edgeVertical);

	float luma1 = isHorizontal ? lumaDown : lumaLeft;
	float luma2 = isHorizontal ? lumaUp : lumaRight;
	float gradient1 = luma1 - lumaCenter;
	float gradient2 = luma2 - lumaCenter;
	bool is1Steepest = abs(gradient1) >= abs(gradient2);
	float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

	float stepLength = isHorizontal ? u_invSize.y : u_invSize.x;
	float lumaLocalAverage = 0.0;
	if(is1Steepest) {
		stepLength = -stepLength;
		lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
	} else {
		lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
	}

	vec2 currentUv = uv;
	if(isHorizontal) {
		currentUv.y += stepLength * 0.5;
	} else {
		currentUv.x += stepLength * 0.5;
	}

	vec2 offset = isHorizontal ? vec2(u_invSize.x, 0.0) : vec2(0.0, u_invSize.y);
	vec2 uv1 = currentUv - offset;
	vec2 uv2 = currentUv + offset;
	float lumaEnd1 = luma(sceneAt(uv1)) - lumaLocalAverage;
	float lumaEnd2 = luma(sceneAt(uv2)) - lumaLocalAverage;
	bool reached1 = abs(lumaEnd1) >= gradientScaled;
	bool reached2 = abs(lumaEnd2) >= gradientScaled;
	bool reachedBoth = reached1 && reached2;
	if(!reached1) {
		uv1 -= offset;
	}
	if(!reached2) {
		uv2 += offset;
	}

	if(!reachedBoth) {
		for(int i = 2; i < ITERATIONS; i++) {
			if(!reached1) {
				lumaEnd1 = luma(sceneAt(uv1)) - lumaLocalAverage;
			}
			if(!reached2) {
				lumaEnd2 = luma(sceneAt(uv2)) - lumaLocalAverage;
			}
			reached1 = abs(lumaEnd1) >= gradientScaled;
			reached2 = abs(lumaEnd2) >= gradientScaled;
			reachedBoth = reached1 && reached2;
			float quality = (i < 5) ? 1.5 : ((i < 7) ? 2.0 : 4.0);
			if(!reached1) {
				uv1 -= offset * quality;
			}
			if(!reached2) {
				uv2 += offset * quality;
			}
			if(reachedBoth) {
				break;
			}
		}
	}

	float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
	float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
	bool isDirection1 = distance1 < distance2;
	float distanceFinal = min(distance1, distance2);
	float edgeThickness = (distance1 + distance2);
	float pixelOffset = -distanceFinal / edgeThickness + 0.5;

	bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
	bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
	float finalOffset = correctVariation ? pixelOffset : 0.0;

	float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
	float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
	float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
	float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * SUBPIXEL_QUALITY;
	finalOffset = max(finalOffset, subPixelOffsetFinal);

	vec2 finalUv = uv;
	if(isHorizontal) {
		finalUv.y += finalOffset * stepLength;
	} else {
		finalUv.x += finalOffset * stepLength;
	}
	return sceneAt(finalUv);
}

void main() {
	if(u_debug == 1) {
		fragColor = vec4(texture(u_aoTexture, v_uv).rrr, 1.0);
		return;
	} else if(u_debug == 2) {
		fragColor = vec4(texture(u_bloomTexture, v_uv).rgb, 1.0);
		return;
	} else if(u_debug == 3) {
		fragColor = vec4(texture(u_volumeTexture, v_uv).rgb * 4.0, 1.0);
		return;
	}
	vec3 c = (u_fxaa != 0) ? fxaa(v_uv) : sceneAt(v_uv);
	fragColor = vec4(c, 1.0);
}
