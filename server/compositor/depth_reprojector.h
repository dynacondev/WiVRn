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
 * along with this program.  If not, see <https://www.gnu.org/>.
 */

#pragma once

#include "vk/allocation.h"

#include "xrt/xrt_defines.h"

#include <array>
#include <vulkan/vulkan_raii.hpp>

// UBO mirror of server/compositor/shaders/reproject_splat.comp:
// source clip -> world -> target clip. mat4 arrays are column-major on
// both sides: xrt_matrix_4x4::v copies straight across. The
// inverse-view * inverse-projection product is folded on the CPU.
struct reproject_splat_ubo
{
	xrt_matrix_4x4 render_inv_vp;
	xrt_matrix_4x4 warp_vp;
};
static_assert(sizeof(reproject_splat_ubo) == 128);

// UBO mirror of server/compositor/shaders/reproject_pull.comp:
// target clip -> world -> source clip, plus the hole-fill radius. The
// products are folded on the CPU.
struct reproject_pull_ubo
{
	xrt_matrix_4x4 dst_inv;
	xrt_matrix_4x4 src_vp;
	// Packs as one vec4 in std140 (float arrays stride to 16): hole
	// radius at +128, 12 pad bytes after. Must stay byte-exact with
	// the PullUBO block.
	float hole_radius;
	float pad[3];
};
static_assert(sizeof(reproject_pull_ubo) == 144);

// Push block mirror of reproject_resolve.comp.
struct reproject_resolve_push
{
	float color_rect[2][4]; // per-eye normalized sub-image rects
	float depth_rect[2][4];
	float color_flip; // 1 = mirror color rows (Vulkan-ordered source)
	float depth_flip; // 1 = mirror depth rows (Vulkan-ordered source)
	float src_reverse_z; // 0 = standard depth, 1 = reverse-Z source (flip 1-z)
	float pad;
};
static_assert(sizeof(reproject_resolve_push) == 80);

namespace wivrn
{
struct vk_bundle;

// Server-side 6DoF reprojection for missed frame deadlines.
// All-compute backward warp in two passes: splat forwards each
// retained source pixel's depth to the target pose (atomic min wins =
// foreground), pull warps color backward per target pixel through the
// splatted depth. Disoccluded pixels keep black; the caller repeats the
// last frame instead when the input is degenerate.
//
// The last real frame is held in compositor-owned retained images
// (refreshed by refresh_retained on every depth commit), so synthetic
// frames warp from held sources even after the app recycles its
// swapchains. Warp targets are plain UNORM and sampled raw: the chain
// introduces no color conversion of its own.
//
// One instance owns both eyes' targets, retained sources and compute
// pipelines. Runs on the compositor queue with no graphics capability
// required. All recording happens on the compositor's commit thread.
class depth_reprojector
{
public:
	// Worst-case cost of the pull hole fill grows with the square of
	// this radius per hole pixel: never raise it without measuring
	// worst-case warp GPU time on the server GPU first.
	static constexpr uint32_t kHoleRadius = 8;

	depth_reprojector(vk_bundle & vk, vk::Extent2D target_size);

	// Copies the current real frame into the retained sources.
	// Norm-rects map each eye's sub-image; the flip flags select the
	// per-plane row order (true = caller rows are Vulkan-ordered, top
	// row first). Both are caller-specified: color and depth may
	// disagree (they do in this pipeline).
	void refresh_retained(
	        vk::raii::Device & device,
	        vk::raii::CommandBuffer & cmd,
	        std::array<vk::ImageView, 2> color,
	        std::array<vk::ImageView, 2> depth,
	        std::array<std::array<float, 4>, 2> color_rect,
	        std::array<std::array<float, 4>, 2> depth_rect,
	        bool flip_color,
	        bool flip_depth);

	// Warps one eye's retained source (render pose/params) to
	// dst_pose/dst_fov, recording into cmd. Output ends in
	// SHADER_READ_ONLY_OPTIMAL, ready to feed foveation (see
	// get_views). Returns false on degenerate input (caller falls
	// back: no synthetic frame). Recorded work is exception-free by
	// construction: pools, sets and pipelines are preallocated, input
	// is validated before anything is recorded.
	bool reproject(
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
	// Depth encoding at construction (config source-reverse-z):
	// reverse-Z sources are converted once in the resolve pass. Read
	// once here so the hot path never touches the config file.
	const bool reverse_z;
	// Hole-fill radius at construction (config hole-fill-radius,
	// clamped 0-kHoleRadius): 0 renders disocclusions black directly.
	const uint32_t hole_fill_radius;

	vk::raii::Sampler color_sampler = nullptr;
	vk::raii::Sampler depth_sampler = nullptr;

	// Retained last-real-frame sources: full-eye color (UNORM) and raw
	// depth (R32F), 2 layers (eyes). SHADER_READ_ONLY except across
	// refresh_retained.
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
	// each warp). SHADER_READ_ONLY (targets) / GENERAL (scratch)
	// between warps. Per-view: the two eyes warp independently, so a
	// view-0 warp must not mark view 1's resources transitioned.
	std::array<bool, 2> warp_targets_ready = {false, false};
	std::array<image_allocation, 2> target;
	std::array<vk::raii::ImageView, 2> target_view = {nullptr, nullptr};
	std::array<image_allocation, 2> splat_depth;
	std::array<vk::raii::ImageView, 2> splat_view = {nullptr, nullptr};

	// Splat: depth sampler + splat UBO + splat storage image.
	// Pull: color sampler + pull UBO + splat storage image (read) +
	// color storage target (write). Sets are written once at
	// construction; only the UBO contents change per warp.
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
};
} // namespace wivrn
