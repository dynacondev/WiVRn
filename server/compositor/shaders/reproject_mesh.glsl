/*
 * Mesh-based 6DoF reprojection shader.
 *
 * The warp math is a Vulkan/GLSL port of the mesh algorithm from OpenWarp
 * (https://github.com/Zee2/openwarp), reproduced under its license:
 *
 * Copyright (c) 2020 Finn Sinclair.  All rights reserved.
 *
 * Developed by: Finn Sinclair
 *               University of Illinois at Urbana-Champaign
 *               finnsinclair.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal with
 * the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to
 * do so, subject to the following conditions:
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimers.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimers in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the names of Finn Sinclair, University of Illinois at Urbana-Champaign,
 *   nor the names of its contributors may be used to endorse or promote products
 *   derived from this Software without specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
 * SOFTWARE.
 *
 * WiVRn port notes (Copyright (C) 2026, GPL-3.0-or-later as part of WiVRn):
 * - in_position/in_uv attributes are replaced by a procedural grid from
 *   gl_VertexIndex: no vertex buffers, one shared index buffer.
 * - No mat3 constructors or matrix ternaries anywhere: Adreno-class
 *   drivers miscompile those in multiview fragment stages, and the same
 *   scalar discipline keeps this shader portable. All matrix math is
 *   mat4 * vec4 with scalar depth handling.
 *
 * Single file compiled twice, mirroring client/shaders/reprojection.glsl:
 * VERT_SHADER selects the vertex stage (forward warp of a procedural mesh
 * grid), FRAG_SHADER the fragment stage (plain color tap).
 */

#version 450

#ifdef VERT_SHADER

// UBO mirror: see depth_reprojector.h. mat4 arrays are column-major on
// both sides; xrt_matrix_4x4::v copies straight across.
layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform ReprojectUBO
{
	mat4 render_inv_p; // inverse of the source (render-pose) projection
	mat4 render_inv_v; // inverse of the source view (camera-to-world)
	mat4 warp_vp; // target projection * target view
	vec4 color_rect; // source color sub-image: normalized offset.xy, extent.xy
	vec4 depth_rect; // source depth sub-image: normalized offset.xy, extent.xy
	float bleed_radius; // edge-bleed radius in depth-UV units
	float bleed_tolerance; // foreground clamp tolerance (depth 0..1 units)
} ubo;

layout(push_constant) uniform Push
{
	uvec2 grid; // mesh quads: width, height
} pc;

layout(location = 0) out vec2 out_color_uv;

void main()
{
	uint cols = pc.grid.x + 1u;
	uint ix = gl_VertexIndex % cols;
	uint iy = gl_VertexIndex / cols;
	vec2 guv = vec2(ix, iy) / vec2(pc.grid);

	vec2 color_uv = ubo.color_rect.xy + guv * ubo.color_rect.zw;
	vec2 depth_uv = ubo.depth_rect.xy + guv * ubo.depth_rect.zw;

	// Source depth is standard 0..1 nonlinear (GL convention).
	float z = textureLod(depth_tex, depth_uv, 0.0).x;

	// Foreground voting around discontinuities: the minimum of the
	// cross + diagonal neighbourhood. Scalar min() chain, no branching.
	float cross_h = min(textureLod(depth_tex, depth_uv - vec2(ubo.bleed_radius, 0.0), 0.0).x,
	    textureLod(depth_tex, depth_uv + vec2(ubo.bleed_radius, 0.0), 0.0).x);
	float cross_v = min(textureLod(depth_tex, depth_uv - vec2(0.0, ubo.bleed_radius), 0.0).x,
	    textureLod(depth_tex, depth_uv + vec2(0.0, ubo.bleed_radius), 0.0).x);
	float diag = min(textureLod(depth_tex, depth_uv - vec2(ubo.bleed_radius, ubo.bleed_radius), 0.0).x,
	    textureLod(depth_tex, depth_uv + vec2(ubo.bleed_radius, ubo.bleed_radius), 0.0).x);
	float outlier = min(min(cross_h, cross_v), diag);

	float z_ndc = z * 2.0 - 1.0;
	float outlier_ndc = outlier * 2.0 - 1.0;
	// Branchless foreground clamp (OpenWarp: if (z - outlier > tol)).
	float use_outlier = step(ubo.bleed_tolerance, z_ndc - outlier_ndc);
	z_ndc = mix(z_ndc, min(outlier_ndc, 0.99), use_outlier);
	z_ndc = min(z_ndc, 0.99);

	// Unproject (source clip -> view -> world), then reproject to the
	// target clip. mat4 * vec4 only, depth handled as scalars above.
	vec4 clip_pos = vec4(depth_uv * 2.0 - 1.0, z_ndc, 1.0);
	vec4 frag_viewspace = ubo.render_inv_p * clip_pos;
	vec4 frag_worldspace = ubo.render_inv_v * frag_viewspace;
	vec4 result = ubo.warp_vp * frag_worldspace;

	result /= abs(result.w);
	gl_Position = result;
	out_color_uv = color_uv;
}

#endif

#ifdef FRAG_SHADER

layout(set = 0, binding = 0) uniform sampler2D color_tex;

layout(location = 0) in vec2 in_color_uv;

layout(location = 0) out vec4 out_color;

void main()
{
	// Forward-warped mesh carries the color lookup per vertex; the
	// fragment is a plain tap. Uncovered pixels keep the clear color.
	out_color = texture(color_tex, in_color_uv);
}

#endif
