// ArxModern ray tracing: walking the level's bounding volume hierarchy (scene/RayScene.cpp).
//
// Shared by the traced passes (reflect_rt.frag, ...), which are built with a "#version 430"
// prelude: this file carries no version line. Needs shader storage buffers (OpenGL 4.3).
//
// A node is two vec4: (min.xyz, leftFirst), (max.xyz, count); count > 0 = leaf over the
// triangles [leftFirst, leftFirst + count), count == 0 = inner node with the children
// leftFirst and leftFirst + 1. A triangle is three vec4: v0, e1 = v1 - v0 (w = |e1 x e2|), e2 = v2 - v0. Its
// attributes are three more: (uv0, uv1), (uv2, layer | flags << 16, color0), (color1, color2,
// 0, 0) - integers stored as their bit patterns, colors as packed RGBA bytes (the static light
// of the vertices), layer = the texture in u_rtTextures.

layout(std430, binding = 1) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 2) readonly buffer RtTriangles { vec4 rt_tris[]; };
layout(std430, binding = 3) readonly buffer RtAttributes { vec4 rt_attrs[]; };

uniform sampler2DArray u_rtTextures;

const uint RtAlphaCutout = 1u;
const uint RtWater = 2u;
const uint RtLava = 4u;
const uint RtGlow = 8u;
const uint RtDoubleSided = 16u;

const int RtStackSize = 32;
const float RtInfinity = 1e30;
const float RtGrazing = 0.12; // shadow rays pass surfaces they hit at less than ~7 degrees
const float RtLightClearance = 48.0; // shadow rays ignore what lies this close to the light: the
                                     // prop it sits in (log pile, candelabra, sconce)
const float RtSkip = 4.0;     // ... and stop this far from the receiving surface (overlapping decals)


struct RtHit {
	float t;
	uint tri;
	vec2 bary; // weights of v1 and v2 (v0 gets the rest)
};

uint rtLayer(uint tri) {
	return floatBitsToUint(rt_attrs[tri * 3u + 1u].z) & 0xffffu;
}

uint rtFlags(uint tri) {
	return floatBitsToUint(rt_attrs[tri * 3u + 1u].z) >> 16u;
}

vec2 rtUv(uint tri, vec2 bary) {
	vec4 a = rt_attrs[tri * 3u];
	vec4 b = rt_attrs[tri * 3u + 1u];
	return a.xy * (1.0 - bary.x - bary.y) + a.zw * bary.x + b.xy * bary.y;
}

// The static light of the surface (the vertex colours, interpolated)
vec3 rtColor(uint tri, vec2 bary) {
	vec4 b = rt_attrs[tri * 3u + 1u];
	vec4 c = rt_attrs[tri * 3u + 2u];
	vec3 c0 = unpackUnorm4x8(floatBitsToUint(b.w)).rgb;
	vec3 c1 = unpackUnorm4x8(floatBitsToUint(c.x)).rgb;
	vec3 c2 = unpackUnorm4x8(floatBitsToUint(c.y)).rgb;
	return c0 * (1.0 - bary.x - bary.y) + c1 * bary.x + c2 * bary.y;
}

// Face normal, not normalised
vec3 rtNormal(uint tri) {
	return cross(rt_tris[tri * 3u + 1u].xyz, rt_tris[tri * 3u + 2u].xyz);
}

// Möller-Trumbore. cullBack (shadow rays): a ray leaving a surface through its back (the
// triangles are wound with their normal towards the open space) passes - lights sitting inside
// the geometry; so does a ray grazing a surface - the slightly uneven floors of the levels must
// not shadow themselves under a low light
bool rtIntersectTriangle(uint tri, vec3 origin, vec3 dir, float tMax, bool cullBack, out float t, out vec2 bary) {
	vec3 v0 = rt_tris[tri * 3u].xyz;
	vec4 e1n = rt_tris[tri * 3u + 1u];
	vec3 e1 = e1n.xyz;
	vec3 e2 = rt_tris[tri * 3u + 2u].xyz;
	vec3 p = cross(dir, e2);
	float det = dot(e1, p); // = -dot(dir, normal): < 0 when the ray goes along the normal (back face)
	if(abs(det) < 1e-7) {
		return false;
	}
	if(cullBack) {
		if(det < 0.0 && (rtFlags(tri) & RtDoubleSided) == 0u) {
			return false;
		}
		if(abs(det) < RtGrazing * e1n.w) {
			return false;
		}
	}
	float invDet = 1.0 / det;
	vec3 s = origin - v0;
	float u = dot(s, p) * invDet;
	if(u < 0.0 || u > 1.0) {
		return false;
	}
	vec3 q = cross(s, e1);
	float v = dot(dir, q) * invDet;
	if(v < 0.0 || u + v > 1.0) {
		return false;
	}
	t = dot(e2, q) * invDet;
	if(t <= 0.0 || t >= tMax) {
		return false;
	}
	bary = vec2(u, v);
	return true;
}

// Entry distance of the ray into the box, or RtInfinity when it misses (slab test)
float rtIntersectBox(vec3 bmin, vec3 bmax, vec3 origin, vec3 invDir, float tMax) {
	vec3 t0 = (bmin - origin) * invDir;
	vec3 t1 = (bmax - origin) * invDir;
	vec3 tmin = min(t0, t1);
	vec3 tmax = max(t0, t1);
	float near = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
	float far = min(min(tmax.x, tmax.y), min(tmax.z, tMax));
	return (near <= far) ? near : RtInfinity;
}

// Holes of the colour-keyed textures (grids, fences, cobwebs) let the ray through
bool rtOpaqueAt(uint tri, vec2 bary) {
	if((rtFlags(tri) & RtAlphaCutout) == 0u) {
		return true;
	}
	return textureLod(u_rtTextures, vec3(rtUv(tri, bary), float(rtLayer(tri))), 0.0).a >= 0.5;
}

/*!
 * Nearest hit along the ray within tMax. anyHit: stop at the first surface found (shadow rays).
 * cullBack: see rtIntersectTriangle.
 */
bool rtTrace(vec3 origin, vec3 dir, float tMax, bool anyHit, bool cullBack, out RtHit hit) {
	vec3 invDir = 1.0 / dir; // inf on a zero component is fine for the slab test
	hit.t = tMax;
	hit.tri = 0xffffffffu;
	hit.bary = vec2(0.0);
	uint stack[RtStackSize];
	int sp = 0;
	uint node = 0u;
	float rootT = rtIntersectBox(rt_nodes[0].xyz, rt_nodes[1].xyz, origin, invDir, hit.t);
	if(rootT >= RtInfinity) {
		return false;
	}
	while(true) {
		vec4 a = rt_nodes[node * 2u];
		vec4 b = rt_nodes[node * 2u + 1u];
		uint leftFirst = floatBitsToUint(a.w);
		uint count = floatBitsToUint(b.w);
		if(count > 0u) {
			for(uint i = leftFirst; i < leftFirst + count; i++) {
				float t;
				vec2 bary;
				if(rtIntersectTriangle(i, origin, dir, hit.t, cullBack, t, bary) && rtOpaqueAt(i, bary)) {
					hit.t = t;
					hit.tri = i;
					hit.bary = bary;
					if(anyHit) {
						return true;
					}
				}
			}
		} else {
			uint left = leftFirst;
			uint right = leftFirst + 1u;
			float tLeft = rtIntersectBox(rt_nodes[left * 2u].xyz, rt_nodes[left * 2u + 1u].xyz, origin, invDir, hit.t);
			float tRight = rtIntersectBox(rt_nodes[right * 2u].xyz, rt_nodes[right * 2u + 1u].xyz, origin, invDir, hit.t);
			if(tLeft > tRight) {
				float tt = tLeft; tLeft = tRight; tRight = tt;
				uint nn = left; left = right; right = nn;
			}
			if(tLeft < RtInfinity) {
				if(tRight < RtInfinity && sp < RtStackSize) {
					stack[sp++] = right;
				}
				node = left;
				continue;
			}
		}
		if(sp == 0) {
			break;
		}
		node = stack[--sp];
	}
	return hit.tri != 0xffffffffu;
}

/*!
 * Shadow ray: is the point lit by (a sample of) a light? Traced from the light, see rtTrace.
 */
bool rtLit(vec3 lightPos, vec3 point) {
	vec3 d = point - lightPos;
	float len = length(d);
	if(len <= RtLightClearance + RtSkip) {
		return true;
	}
	d /= len;
	RtHit hit;
	return !rtTrace(lightPos + d * RtLightClearance, d, len - RtLightClearance - RtSkip, true, true, hit);
}
