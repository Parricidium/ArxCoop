#version 130

// ArxModern lava.
//
// The engine draws lava as a flat polygon with its own texture (already in the scene) and
// used to modulate three scrolling copies of the "enviro" highlight texture on top of it.
// This pass replaces that overlay: the molten veins glow and pulse, the crust between them
// darkens, the surface slowly heaves (the base texture is read back through a wobble), and
// a second draw of the polygons raised above the pool (u_haze != 0) distorts what is seen
// through the hot air.
//
// Inputs from the engine: u_scene / u_depth = the scene as rendered so far (resolved copies),
// u_enviro = the original highlight texture (unit 0) with its three scrolling uv sets.

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
uniform int u_haze;       // 1 = the raised cap (heat haze), 0 = the surface

in vec3 v_worldPos;
in float v_viewDepth;
in float v_interior;
in vec2 v_uv0;
in vec2 v_uv1;
in vec2 v_uv2;

out vec4 fragColor;

// Tunables (a mod can edit this file: F7 reloads it)
const vec3 GlowColor = vec3(1.0, 0.45, 0.08);   // colour of the molten veins
const vec3 HotColor = vec3(1.0, 0.85, 0.5);     // colour of the hottest spots
const float Glow = 1.1;                         // emissive strength
const float Pulse = 0.25;                       // slow breathing of the glow
const float Crust = 0.5;                        // how dark the cooled crust gets
const float Wobble = 0.0025;                    // screen-space heave of the surface texture
const float Haze = 0.012;                       // screen-space distortion of the heat haze
const float HazeSpeed = 1.7;

float linearDepth(float zBuffer) {
	float zNdc = zBuffer * 2.0 - 1.0;
	return u_projection.w / (u_projection.z - zNdc);
}

float sceneDepth(vec2 uv) {
	return linearDepth(texture(u_depth, uv).r);
}

// Screen colour at uv + offset, unless something nearer than this fragment stands there
vec3 sceneBehind(vec2 uv, vec2 offset, float surfaceZ) {
	vec2 uv2 = clamp(uv + offset, u_invSize, 1.0 - u_invSize);
	if(sceneDepth(uv2) < surfaceZ - 1.0) {
		uv2 = uv;
	}
	return texture(u_scene, uv2).rgb;
}

void main() {

	vec2 uv = gl_FragCoord.xy * u_invSize;
	float surfaceZ = linearDepth(gl_FragCoord.z);
	float fade = clamp(v_interior, 0.0, 1.0) * u_strength;

	// The three scrolling layers of the original effect, as a "heat" field
	vec3 l0 = texture(u_enviro, v_uv0).rgb;
	vec3 l1 = texture(u_enviro, v_uv1).rgb;
	vec3 l2 = texture(u_enviro, v_uv2).rgb;
	// The product of the layers is about 0.2 on average: the veins are its high end, the
	// cooled crust its low end
	float product = l0.r * l1.g * l2.b;
	float heat = smoothstep(0.17, 0.42, product);
	float crust = smoothstep(0.16, 0.05, product);
	float fine = l1.r * l2.g * 2.0;

	if(u_haze != 0) {
		// Heat haze: the scene seen through the rising hot air, wavering
		vec2 wave = vec2(sin(v_worldPos.x * 0.05 + u_time * HazeSpeed * 1.3 + fine * 6.0),
		                 cos(v_worldPos.z * 0.045 + u_time * HazeSpeed + heat * 5.0));
		vec2 offset = wave * Haze * fade * (0.4 + 0.6 * heat);
		// Perspective: distort less far away
		offset *= clamp(400.0 / max(surfaceZ, 1.0), 0.2, 1.0);
		fragColor = vec4(sceneBehind(uv, offset, surfaceZ), 1.0);
		return;
	}

	// The surface: the base texture through a slow heave
	vec2 wobble = vec2(sin(u_time * 0.7 + v_worldPos.z * 0.02), cos(u_time * 0.5 + v_worldPos.x * 0.02)) * Wobble * fade;
	vec3 base = sceneBehind(uv, wobble, surfaceZ);

	// Glowing veins where the layers coincide, pulsing; darker crust where they do not
	float pulse = 1.0 + Pulse * sin(u_time * 1.1 + product * 12.0);
	float hot = heat * heat;
	vec3 color = base * (1.0 - Crust * crust) * (1.0 + Glow * heat * pulse);
	color += mix(GlowColor, HotColor, hot) * (hot * Glow * 0.5 * pulse);

	// Fog dims the glow as it does the scene
	if(u_fogEnabled != 0) {
		float fog = clamp((u_fogRange.y - v_viewDepth) / (u_fogRange.y - u_fogRange.x), 0.0, 1.0);
		color = mix(base, color, fog);
	}

	fragColor = vec4(mix(base, color, u_strength), 1.0);
}
