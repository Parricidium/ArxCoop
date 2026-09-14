#version 130

// ArxModern shadow map pass: stores the distance to the light, normalised by its range,
// in the red channel. The alpha channel carries the texture alpha so that the alpha test
// still cuts out grates and foliage.

uniform vec3 u_lightPos;
uniform float u_lightFallend;
uniform sampler2D u_texture0;
uniform int u_textured;
uniform int u_transform; // 0 = entity geometry, 1 = level geometry
uniform float u_lightOwner; // entity index carrying the light (-1 = none)

// The entity carrying a light never occludes it: a wall torch's bracket, a candle's stick, a
// character with a torch at the belt (the light sits inside its body) - and everything hanging
// from it (weapon, shield, the torch itself). Without this half the carrier would shadow the
// other half and the room, or a torch lit by a spell would give no light at all.
//
// Geometry this close to the light does not occlude it either. Entities: the hand casting a
// spell, an object standing right against a flame. Level: the engine places entity ignition
// lights 30 units above the flame, which for a wall torch under a beam or a low ceiling is
// inside the level geometry - without this the whole room would be in shadow.
const float ENTITY_NEAR = 30.0;
const float LEVEL_NEAR = 22.0;

in vec3 v_worldPos;
in vec2 v_texcoord0;
in float v_caster;

out vec4 fragColor;

void main() {
	if(u_transform == 0 && u_lightOwner >= 0.0 && abs(v_caster - u_lightOwner) < 0.5) {
		discard;
	}
	float alpha = (u_textured != 0) ? texture(u_texture0, v_texcoord0).a : 1.0;
	float worldDist = length(v_worldPos - u_lightPos);
	if(worldDist < ((u_transform == 0) ? ENTITY_NEAR : LEVEL_NEAR)) {
		discard;
	}
	fragColor = vec4(worldDist / u_lightFallend, 0.0, 0.0, alpha);
}
