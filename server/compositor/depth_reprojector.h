/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#pragma once

#include "vk/allocation.h"

#include "xrt/xrt_defines.h"

#include <array>
#include <vulkan/vulkan_raii.hpp>

// UBO mirror of server/compositor/shaders/reproject_mesh.glsl.
// mat4 arrays are column-major on both sides: xrt_matrix_4x4::v copies
// straight across. All matrix math is mat4 * vec4 with scalar depth
// handling: no mat3 constructors or matrix ternaries anywhere (see
// AGENTS.md Adreno notes; keeps the shader portable anyway).
struct reproject_ubo_data
{
	xrt_matrix_4x4 render_inv_p;
	xrt_matrix_4x4 render_inv_v;
	xrt_matrix_4x4 warp_vp;
	// Normalized sub-image rects: offset.xy, extent.xy.
	float color_rect[4];
	float depth_rect[4];
	float bleed_radius;
	float bleed_tolerance;
	// 0 = GL-ordered source (row 0 = frustum bottom, e.g. xrizer),
	// 1 = Vulkan-ordered source. Sampling V and output Y flip together
	// so the warped target is always GL-ordered (foveated with flip).
	float src_flip_y;
	float pad;
};
static_assert(sizeof(reproject_ubo_data) == 240);

namespace wivrn
{
struct vk_bundle;

// Server-side 6DoF reprojection (async spacewarp for missed frame
// deadlines). Forward-warp mesh pass, Vulkan port of the OpenWarp mesh
// algorithm: each mesh vertex unprojects its source depth sample to the
// world and reprojects to the target pose; the fragment is a plain
// color tap. Uncovered pixels keep the renderpass clear color.
//
// One instance owns both eyes' targets, pipelines and mesh. Use is
// single-threaded (compositor layer_commit thread).
class depth_reprojector
{
public:
	struct frame_source
	{
		vk::ImageView color;
		vk::ImageView depth;
		// Normalized sub-image rects: offset.xy, extent.xy. Must have
		// positive extent (full-eye from xrizer); anything else makes
		// reproject() decline.
		std::array<float, 4> color_rect;
		std::array<float, 4> depth_rect;
		xrt_pose pose;
		xrt_fov fov;
		float near_z;
		float far_z;
		// 0 = GL-ordered source rows, 1 = Vulkan-ordered.
		float src_flip_y = 0;
	};

	depth_reprojector(vk_bundle & vk, vk::Extent2D target_size);

	// False when the compositor queue family cannot do graphics: the
	// caller must fall back to repeating the previous frame.
	bool supported() const
	{
		return graphics_supported;
	}

	uint32_t mesh_width() const
	{
		return mesh_w;
	}
	uint32_t mesh_height() const
	{
		return mesh_h;
	}

	// Warps one eye's source (render pose) to dst_pose/dst_fov, recording
	// into cmd. Output ends in SHADER_READ_ONLY_OPTIMAL, ready to feed
	// foveation (see get_views). Returns false on degenerate input
	// (caller falls back: no synthetic frame).
	bool reproject(
	        vk::raii::Device & device,
	        vk::raii::CommandBuffer & cmd,
	        int view,
	        const frame_source & src,
	        const xrt_pose & dst_pose,
	        const xrt_fov & dst_fov);

	std::array<vk::ImageView, 2> get_views();

private:
	const bool graphics_supported;
	const uint32_t mesh_w;
	const uint32_t mesh_h;
	const float bleed_radius;
	const float bleed_tolerance;

	vk::raii::Sampler clamp_to_border_black;
	vk::raii::Sampler clamp_to_edge;

	// Per-eye warped color targets: mutable-format images with an UNORM
	// attachment view (render into, no sRGB conversion on store) and an
	// SRGB sampled view (same convention as layer_squasher targets).
	std::array<image_allocation, 2> target;
	std::array<vk::raii::ImageView, 2> target_view_unorm;
	std::array<vk::raii::ImageView, 2> target_view_srgb;

	vk::raii::RenderPass renderpass;
	std::array<vk::raii::Framebuffer, 2> framebuffers;

	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::PipelineLayout layout;
	vk::raii::Pipeline pipeline;
	vk::raii::DescriptorPool descriptor_pool;
	std::array<vk::DescriptorSet, 2> descriptor_sets;

	std::array<buffer_allocation, 2> ubo;
	buffer_allocation index_buffer;
};
} // namespace wivrn
