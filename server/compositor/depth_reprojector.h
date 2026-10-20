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

// UBO mirror of server/compositor/shaders/reproject_splat.comp:
// source clip -> world -> target clip. mat4 arrays are column-major on
// both sides: xrt_matrix_4x4::v copies straight across.
struct reproject_splat_ubo
{
	xrt_matrix_4x4 render_inv_p;
	xrt_matrix_4x4 render_inv_v;
	xrt_matrix_4x4 warp_vp;
};
static_assert(sizeof(reproject_splat_ubo) == 192);

// UBO mirror of server/compositor/shaders/reproject_pull.comp:
// target clip -> world -> source clip, plus the hole-fill radius.
struct reproject_pull_ubo
{
	xrt_matrix_4x4 dst_inv_p;
	xrt_matrix_4x4 dst_inv_v;
	xrt_matrix_4x4 src_p;
	xrt_matrix_4x4 src_v;
	float hole_radius;
	float pad[3];
};
static_assert(sizeof(reproject_pull_ubo) == 272);

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
// deadlines). All-compute backward warp in the OpenWarp algorithm
// family: pass 1 splats each retained source pixel's depth forward to
// the target pose (atomic min wins = foreground), pass 2 pulls color
// backward per target pixel through the splatted depth. Holes
// (disoccluded pixels) are filled by an expanding-ring search for the
// nearest valid splat, else keep the clear color.
//
// The last real frame is held in compositor-owned retained images
// (refreshed by refresh_retained on every depth commit), so any number
// of synthetic frames can warp from held sources even after the app
// recycles its swapchains. Warp targets are plain UNORM and sampled
// raw: the chain introduces no color conversion of its own, values
// match the direct sampling path exactly.
//
// One instance owns both eyes' targets, retained sources and compute
// pipelines. Runs on the compositor queue with no graphics capability
// required. Use is single-threaded (compositor layer_commit thread).
class depth_reprojector
{
public:
	depth_reprojector(vk_bundle & vk, vk::Extent2D target_size);

	// Number of target pixels that found no valid source (monotonic).
	// drain_hole_count() returns the delta since the last call.
	uint32_t drain_hole_count();

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
	const uint32_t hole_radius;

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

	// Per-eye warped color targets (plain UNORM, sampled raw) plus
	// per-eye splatted foreground-depth scratch (R32_UINT, cleared
	// each warp). GENERAL except when sampled downstream.
	bool warp_targets_ready = false;
	std::array<image_allocation, 2> target;
	std::array<vk::raii::ImageView, 2> target_view = {nullptr, nullptr};
	std::array<image_allocation, 2> splat_depth;
	std::array<vk::raii::ImageView, 2> splat_view = {nullptr, nullptr};

	// Splat: depth sampler + splat UBO + splat storage image.
	// Pull: color sampler + pull UBO + splat storage image +
	// hole counter + color storage target.
	vk::raii::DescriptorSetLayout splat_ds_layout = nullptr;
	vk::raii::PipelineLayout splat_layout = nullptr;
	vk::raii::Pipeline splat_pipeline = nullptr;
	vk::raii::DescriptorSetLayout pull_ds_layout = nullptr;
	vk::raii::PipelineLayout pull_layout = nullptr;
	vk::raii::Pipeline pull_pipeline = nullptr;
	vk::raii::DescriptorPool warp_pool = nullptr;
	std::array<vk::DescriptorSet, 2> splat_sets;
	std::array<vk::DescriptorSet, 2> pull_sets;

	std::array<buffer_allocation, 2> splat_ubo;
	std::array<buffer_allocation, 2> pull_ubo;

	// Monotonic unfilled-pixel count (host-visible, drained for logs).
	buffer_allocation hole_count;
	uint32_t last_hole_total = 0;
};
} // namespace wivrn
