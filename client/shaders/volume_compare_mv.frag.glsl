/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#version 450

#extension GL_EXT_multiview : require

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// Depth-volume flatten with rotation + B2 lateral translation: the sensor
// tap is remapped through the capture<->display relative rotation, then
// nudged by T/depth for sideways parallax (forward/radial + depth-value
// terms are NOT handled here — that residual is the experiment).
// Identity reproduces the direct tap. Adaptive sensor<v band (coverage
// AA: width follows the local depth slope, flat interiors stay binary),
// far / no-data hidden, out-of-frustum hidden. Both taps are LINEAR-filtered
// (host sampler): the bilinear depth ramp feeds the slope estimate.
// Branchless by construction (derivatives need full-quad execution).
//
// The remap rides push constants as 8 NAMED vec4 members (not a GLSL
// array: push-constant arrays have driver-defined packing, while
// consecutive vec4 members lay out exactly — 16B each, 128B total).
// Per-eye homography columns: eye e uses h[3e..3e+2]. Per-eye lateral
// correction: eye e uses corr[e].xy = T (UV per 1/m), corr[e].z/.w =
// (a, b) with 1/depth = a*D + b (duplicated; D is sensor-compatible).
// (History: the original failure was the mat3 constructor in the math
// below, never the transport — UBO, texel-buffer and sampled-image
// variants all behaved identically once mat3 was gone. Push is kept as
// the simplest form: no buffers, views, copies or barriers per frame.)
layout(push_constant) uniform Push
{
	vec4 h0;
	vec4 h1;
	vec4 h2;
	vec4 h3;
	vec4 h4;
	vec4 h5;
	vec4 corr0;
	vec4 corr1;
}
pc;

layout(set = 0, binding = 0) uniform sampler2DArray sensor_tex;
layout(set = 0, binding = 1) uniform sampler2DArray volume_tex;

void main()
{
	// Branchless eye select (mix, never a ternary/mat3): derivatives
	// below need every quad lane on the same code path.
	float vi = float(gl_ViewIndex);
	float m = step(0.5, vi);
	vec4 c0 = mix(pc.h0, pc.h3, m);
	vec4 c1 = mix(pc.h1, pc.h4, m);
	vec4 c2 = mix(pc.h2, pc.h5, m);
	vec4 corr = mix(pc.corr0, pc.corr1, m);
	vec2 T = corr.xy;
	float ka = corr.z;
	float kb = corr.w;

	float px = c0.x * uv.x + c1.x * uv.y + c2.x;
	float py = c0.y * uv.x + c1.y * uv.y + c2.y;
	float pz = c0.z * uv.x + c1.z * uv.y + c2.z;
	vec2 s = vec2(px, py) / max(pz, 1e-6);
	// Validity folded into a flag (no early returns: dFdx/dFdy are only
	// defined when the whole quad runs every fetch). Out-of-range
	// fetches hit clamp-to-edge and die in the flag.
	float ok = step(1e-6, pz)
	         * step(0.0, s.x) * step(s.x, 1.0)
	         * step(0.0, s.y) * step(s.y, 1.0);

	// B2 lateral: one depth-dependent nudge from the rotation-only spot.
	// T maps 1/depth (1/m) to sensor-UV; k = 1/depth from D (affine, no
	// divide). Far/no-data (D~1) yields k~0: the second tap lands on the
	// same texel (cache hit). Forward/radial + depth-value terms are
	// deliberately absent here.
	vec2 suv0 = s;
	float d0 = texture(sensor_tex, vec3(suv0, vi)).r;
	float v = texture(volume_tex, vec3(uv, vi)).r;
	// k guard: at depth discontinuities the rotation-only spot can land
	// on background (k_sens ~ 0, no shift, one-sided edge erosion). The
	// live volume depth is always on the correct side of the edge, so
	// fall back to it. Interior pixels are unaffected (the nearer
	// sensor/hand depth wins the max); v == 0 contributes 0 below
	// regardless of k. Clamped to a 5cm floor.
	float k_sens = ka * d0 + kb;
	float k_vol = (v > 0.0) ? ka * v + kb : 0.0;
	float k = min(max(k_sens, k_vol), 20.0);
	vec2 suv1 = suv0 + T * k;
	float ok1 = ok
	          * step(0.0, suv1.x) * step(suv1.x, 1.0)
	          * step(0.0, suv1.y) * step(suv1.y, 1.0);
	float d = texture(sensor_tex, vec3(suv1, vi)).r;

	// Adaptive e (screen-space AA width): the band follows the local
	// depth slope, so flat interiors stay binary-solid (no transparency:
	// e ~ 0 where nothing varies) while edge texels spanning a depth gap
	// get a full-texel gradient (coverage-like, self-tuned per edge and
	// stable across depths — a fixed D-wide e can never do both). c ~ 0.5
	// starts the gradient at roughly the edge width; the 1e-4 floor keeps
	// smoothstep defined on perfectly flat fields.
	float gx = abs(dFdx(d)) + abs(dFdx(v));
	float gy = abs(dFdy(d)) + abs(dFdy(v));
	float e = 0.5 * (gx + gy) + 1e-4;
	// Far (d ~= 1) and empty-volume (v == 0) stay hard-excluded; the band
	// only feathers the d~v boundary itself.
	float inside = (1.0 - smoothstep(v - e, v + e, d))
	             * ok1 * step(1e-6, v) * (1.0 - step(0.9999, d));
	out_color = vec4(inside);
}
