// ArxModern volumetric fog, computed at half resolution from the scene depth.
//
// The air of the levels becomes a thin, drifting haze: along the ray of every pixel the light
// of the scene's torches, sconces and spells is scattered towards the camera (light shafts and
// halos), and what lies behind the haze is dimmed by it. With the ray tracing built in (ARX_RT,
// rt_common.glsl prepended) the shafts are shadowed by the level geometry: the light of a
// sconce round a corner does not glow through the wall.
//
// Output: rgb = in-scattered light, a = transmittance (post_final.frag composes
// scene * a + rgb). u_projection = (proj[0][0], proj[1][1], Q, Q * near) as in post_ssao.frag.

#define MAX_LIGHTS 128

uniform sampler2D u_depth;
uniform vec4 u_projection;
uniform mat4 u_invView;     // view -> world
uniform vec3 u_cameraPos;
uniform float u_density;    // 0..1, the "Haze" option
uniform float u_time;
uniform int u_lightCount;   // every light of the scene (the static sconces too), like the water pass
uniform vec4 u_lightPos[MAX_LIGHTS];   // xyz, fallstart
uniform vec4 u_lightColor[MAX_LIGHTS]; // rgb, fallend
uniform int u_shadows;      // 1: trace the shafts' shadows (ARX_RT only)
// The shadow cube maps of the first lights (the characters and objects standing in the light -
// and the level too without the ray tracing): distance of the nearest occluder from the light,
// normalised by its range, as in legacy.frag. u_lightShadow[i] = which cube map, or -1
uniform int u_lightShadow[MAX_LIGHTS];
uniform samplerCube u_shadow0;
uniform samplerCube u_shadow1;
uniform samplerCube u_shadow2;
uniform samplerCube u_shadow3;

in vec2 v_uv;
out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const int Steps = 12;               // samples along each ray
const float MaxRange = 3000.0;      // world units of haze in front of the camera
const float RangeFade = 0.5;        // fraction of the range from which the haze thins out
const float Extinction = 0.00015;   // per world unit at density 1: how much the haze dims what is behind
const float Scatter = 0.0011;       // per world unit at density 1: how much light it throws back
const float Ambient = 0.012;        // faint glow of the haze where no light reaches
const float Anisotropy = 0.35;      // Henyey-Greenstein g: > 0 scatters forward (halos round the lights)
const float NoiseScale = 0.0035;    // world units -> noise; smaller = larger wisps
const float NoiseAmount = 0.75;     // 0 = uniform haze, 1 = strongly wispy
const float Drift = 0.05;           // wisps drifting speed
const int MaxLightsPerRay = 12;     // the strongest lights on the ray only
const float HiddenWeight = 0.25;    // ranking weight kept by a light hidden where the ray passes closest
const float MinFalloff = 0.35;      // fraction of a light's range over which it fades at least

float linearDepth(vec2 uv) {
	float zNdc = texture(u_depth, uv).r * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

float hash(vec3 p) {
	p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.23));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Smooth value noise, 0..1
float noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(hash(i), hash(i + vec3(1.0, 0.0, 0.0)), f.x),
	               mix(hash(i + vec3(0.0, 1.0, 0.0)), hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
	           mix(mix(hash(i + vec3(0.0, 0.0, 1.0)), hash(i + vec3(1.0, 0.0, 1.0)), f.x),
	               mix(hash(i + vec3(0.0, 1.0, 1.0)), hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

float density(vec3 p) {
	vec3 q = p * NoiseScale + vec3(u_time * Drift, u_time * Drift * 0.3, 0.0);
	float n = noise(q) * 0.65 + noise(q * 2.7 + 3.1) * 0.35;
	return u_density * mix(1.0, n * 1.6, NoiseAmount);
}

float phase(float cosTheta) {
	float g = Anisotropy;
	float d = 1.0 + g * g - 2.0 * g * cosTheta;
	return (1.0 - g * g) / (4.0 * 3.14159265 * d * sqrt(d));
}

// The engine's falloff (constant to fallstart, linear to zero at fallend), but never steeper
// than over a third of the range: some level lights have fallstart right at fallend, which
// would show as a hard-edged ball of haze in the air
float falloff(float dist, float fallstart, float fallend) {
	float width = max(fallend - fallstart, fallend * MinFalloff);
	float f = clamp((fallend - dist) / width, 0.0, 1.0);
	return f * f * (3.0 - 2.0 * f); // eased: a linear edge integrates into a visible rim
}

float shadowDistance(int map, vec3 dir) {
	if(map == 0) {
		return texture(u_shadow0, dir).r;
	} else if(map == 1) {
		return texture(u_shadow1, dir).r;
	} else if(map == 2) {
		return texture(u_shadow2, dir).r;
	}
	return texture(u_shadow3, dir).r;
}

// Whether the point sees the light according to its cube map (what stands in the beam)
bool mapLit(int map, vec3 fromLight, float dist, float fallend) {
	return dist / fallend - 0.01 <= shadowDistance(map, fromLight);
}

// Interleaved gradient noise: offsets the samples per pixel so that the steps do not band
float dither(vec2 p) {
	return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

void main() {

	// The ray of this pixel, in world space, with t = view depth
	vec2 ndc = v_uv * 2.0 - 1.0;
	vec3 dirView = vec3(ndc.x / u_projection.x, ndc.y / u_projection.y, 1.0);
	vec3 dirWorld = mat3(u_invView) * dirView;
	float dirLength = length(dirWorld);
	vec3 dir = dirWorld / dirLength;
	float range = min(linearDepth(v_uv), MaxRange / dirLength);

	// The lights that matter most for this ray: those whose sphere the ray crosses, ranked by
	// their brightness where the ray passes closest. Keeping the first ones found instead
	// showed the spheres of dim distant lights as circles (a bright light lost its slot inside
	// them) and halos that switched off with the view direction.
	int lights[MaxLightsPerRay];
	float weights[MaxLightsPerRay];
	int lightCount = 0;
	for(int i = 0; i < u_lightCount; i++) {
		vec3 toLight = u_lightPos[i].xyz - u_cameraPos;
		float along = clamp(dot(toLight, dir), 0.0, range * dirLength);
		float away = length(toLight - dir * along);
		float fallend = u_lightColor[i].w;
		if(away >= fallend) {
			continue;
		}
		float weight = falloff(away, u_lightPos[i].w, fallend) * dot(u_lightColor[i].rgb, vec3(0.299, 0.587, 0.114));
#ifdef ARX_RT
		// A bright light hidden behind a wall must not crowd out the lanterns that really shine
		// on this ray (it left dark discs): one ray to where it would matter most weighs it down
		// - softly, so that the ranking does not flip at the edge of the wall's shadow
		if(u_shadows != 0 && !rtLit(u_lightPos[i].xyz, u_cameraPos + dir * along)) {
			weight *= HiddenWeight;
		}
#endif
		// Insert in decreasing weight, dropping the weakest beyond MaxLightsPerRay
		int slot = min(lightCount, MaxLightsPerRay - 1);
		if(lightCount < MaxLightsPerRay || weight > weights[slot]) {
			while(slot > 0 && weights[slot - 1] < weight) {
				lights[slot] = lights[slot - 1];
				weights[slot] = weights[slot - 1];
				slot--;
			}
			lights[slot] = i;
			weights[slot] = weight;
			lightCount = min(lightCount + 1, MaxLightsPerRay);
		}
	}

	float dt = range / float(Steps);
	float t = dt * dither(gl_FragCoord.xy);
	float transmittance = 1.0;
	vec3 inscatter = vec3(0.0);
	for(int s = 0; s < Steps; s++) {
		vec3 p = u_cameraPos + dirWorld * t;
		float stepLength = dt * dirLength;
		// The haze thins out towards the end of its range instead of stopping dead: a light
		// standing just beyond the range showed as a hard disc of glow (the rays ending inside
		// its sphere got it, the others did not)
		float rho = density(p) * (1.0 - smoothstep(MaxRange * RangeFade, MaxRange, t * dirLength));
		vec3 light = vec3(Ambient);
		for(int k = 0; k < lightCount; k++) {
			int i = lights[k];
			vec3 toLight = u_lightPos[i].xyz - p;
			float dist = length(toLight);
			float fallend = u_lightColor[i].w;
			if(dist >= fallend) {
				continue;
			}
			float fallstart = u_lightPos[i].w;
			float attenuation = falloff(dist, fallstart, fallend);
			// Closer to the source than fallstart the light gets stronger still (a flame is small)
			attenuation *= 1.0 + 2.0 * clamp(1.0 - dist / fallstart, 0.0, 1.0);
#ifdef ARX_RT
			if(u_shadows != 0 && !rtLit(u_lightPos[i].xyz, p)) {
				continue;
			}
#endif
			int map = u_lightShadow[i];
			if(map >= 0 && !mapLit(map, -toLight, dist, fallend)) {
				continue;
			}
			light += u_lightColor[i].rgb * (attenuation * phase(dot(toLight / dist, -dir)));
		}
		float extinction = exp(-rho * Extinction * stepLength);
		inscatter += transmittance * light * (rho * Scatter * stepLength);
		transmittance *= extinction;
		t += dt;
	}

	fragColor = vec4(inscatter, transmittance);
}
