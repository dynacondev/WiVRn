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
	// Normalized sub-image rects of the retained sources: offset.xy,
	// extent.xy. Always full-eye (identity) by construction.
	float color_rect[4];
	float depth_rect[4];
	float bleed_radius;
	float bleed_tolerance;
	float pad[2];
};
static_assert(sizeof(reproject_ubo_data) == 240);

// Push block mirror of reproject_resolve.comp.
struct reproject_resolve_push
{
	float color_rect[2][4];
	float depth_rect[2][4];
	float src_flip; // 0 = GL-ordered source rows, 1 = Vulkan-ordered
	float src_reverse_z; // 0 = standard depth, 1 = reverse-Z source (flip 1-z)
};
static_assert(sizeof(reproject_resolve_push) == 72);

namespace wivrn
{
struct vk_bundle;

// Server-side 6DoF reprojection (async spacewarp for missed frame
// deadlines). Forward-warp mesh pass, Vulkan port of the OpenWarp mesh
// algorithm: each mesh vertex unprojects its source depth sample to the
// world and reprojects to the target pose; the fragment is a plain
// color tap. Uncovered pixels keep the renderpass clear color.
//
// The last real frame is held in compositor-owned retained images
// (refreshed by refresh_retained on every depth commit), so any number
// of synthetic frames can warp from held sources even after the app
// recycles its swapchains. Warp targets are plain UNORM and sampled
// raw: the chain introduces no color conversion of its own, values
// match the direct sampling path exactly.
//
// One instance owns both eyes' targets, retained sources, pipelines
// and mesh. Use is single-threaded (compositor layer_commit thread).
class depth_reprojector
{
public:
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

	// Copies the current real frame into the retained sources.
	// Norm-rects map each eye's sub-image; flip_y selects the source
	// row order (true = GL-ordered, as submitted by GL runtimes).
	void refresh_retained(
	        vk::raii::Device & device,
	        vk::raii::CommandBuffer & cmd,
	        std::array<vk::ImageView, 2> color,
	        std::array<vk::ImageView, 2> depth,
	        std::array<std::array<float, 4>, 2> color_rect,
	        std::array<std::array<float, 4>, 2> depth_rect,
	        bool flip_y);

	// Warps one eye's retained source (render pose/param) to
	// dst_pose/dst_fov, recording into cmd. Output ends in
	// SHADER_READ_ONLY_OPTIMAL, ready to feed foveation (see
	// get_views). Returns false on degenerate input (caller falls
	// back: no synthetic frame).
	bool reproject(
	        vk::raii::Device & device,
	        vk::raii::CommandBuffer & cmd,
	        int view,
	        const xrt_pose & src_pose,
	        const xrt_fov & src_fov,
	        float near_z,
	        float far_z,
	        const xrt_pose & dst_pose,
	        const xrt_fov & dst_fov);

	std::array<vk::ImageView, 2> get_views();

private:
	const bool graphics_supported;
	const uint32_t mesh_w;
	const uint32_t mesh_h;
	const float bleed_radius;
	const float bleed_tolerance;
	const bool reverse_z;

	vk::raii::Sampler clamp_to_border_black = nullptr;
	vk::raii::Sampler clamp_to_edge = nullptr;

	// Retained last-real-frame sources, GL-ordered by construction:
	// full-eye color (UNORM) and raw depth (R32F), 2 layers (eyes).
	// SHADER_READ_ONLY except across refresh_retained.
	bool retained_initialized = false;
	image_allocation retained_color;
	image_allocation retained_depth;
	vk::raii::ImageView retained_color_view = nullptr;
	vk::raii::ImageView retained_depth_view = nullptr;
	std::array<vk::raii::ImageView, 2> retained_color_eye = {nullptr, nullptr};
	std::array<vk::raii::ImageView, 2> retained_depth_eye = {nullptr, nullptr};

	vk::raii::DescriptorSetLayout resolve_ds_layout = nullptr;
	vk::raii::PipelineLayout resolve_layout = nullptr;
	vk::raii::Pipeline resolve_pipeline = nullptr;
	vk::raii::DescriptorPool resolve_pool = nullptr;
	vk::DescriptorSet resolve_set;

	// Per-eye warped color targets (plain UNORM, sampled raw).
	std::array<image_allocation, 2> target;
	std::array<vk::raii::ImageView, 2> target_view = {nullptr, nullptr};

	vk::raii::RenderPass renderpass = nullptr;
	std::array<vk::raii::Framebuffer, 2> framebuffers = {nullptr, nullptr};

	vk::raii::DescriptorSetLayout ds_layout = nullptr;
	vk::raii::PipelineLayout layout = nullptr;
	vk::raii::Pipeline pipeline = nullptr;
	vk::raii::DescriptorPool descriptor_pool = nullptr;
	std::array<vk::DescriptorSet, 2> descriptor_sets;

	std::array<buffer_allocation, 2> ubo;
	buffer_allocation index_buffer;
};
} // namespace wivrn
