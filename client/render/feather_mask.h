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

#pragma once

// Feathered passthrough window mask.
//
// Per frame (both eyes): rasterize the calibrated mesh silhouette binary
// at full resolution into intermediate A, separable Gaussian blur H into
// B and V into the submitted swapchain image. The blur decouples feather
// width from shape resolution: opaque interiors stay pixel-exact while
// the band is genuinely smooth (robust to the compositor sampling with or
// without filtering). Fixed 9-tap kernel (sigma 2); feather-px maps to
// tap spread, recommended range 4-12 (see docs/configuration.md).
//
// Swapchain images only allow COLOR_ATTACHMENT output, which is why the
// silhouette is a raster pass and not a buffer copy.

#include "render/passthrough_mesh.h"

#include <array>
#include <glm/mat4x4.hpp>
#include <openxr/openxr.h>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

class feather_mask_renderer
{
public:
	feather_mask_renderer(vk::raii::Device & device,
	                      vk::raii::PhysicalDevice & physical_device,
	                      vk::Format format);

	// (Re)upload mesh buffers. Synchronous (device idle on replace); call
	// when idle, e.g. on map change, not per frame.
	void set_soup(const passthrough_mesh::triangle_soup & soup);

	// Record silhouette + blur chain for both eyes into the layers of an
	// acquired swapchain image. No-op when no mesh is set. The image must
	// be unused (UNDEFINED is fine); it is left in GENERAL for the
	// compositor. Feather selects the tier (0 = hard edge raster direct,
	// 1 = full-res blur, 2/4/8 = blur at half/quarter/eighth with exact
	// spread mapping, clamped to 128px); rasterize=false clears only.
	// Opacity is the first-acquisition fade (1 = fully present).
	void record(vk::raii::CommandBuffer & cmd,
	            vk::Image image,
	            vk::Extent2D extent,
	            const std::array<glm::mat4, 2> & mvp,
	            bool rasterize = true,
	            float feather_px = 24.0f,
	            float opacity = 1.0f);

	bool has_mesh() const
	{
		return index_count > 0;
	}

	// Drop cached framebuffers/views (swapchain recreated: old image
	// handles are dead and driver handle recycling could otherwise alias
	// a stale framebuffer onto a new image).
	void reset_targets()
	{
		targets.clear();
	}

private:
	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	vk::Format format;

	std::pair<vk::raii::Buffer, vk::raii::DeviceMemory> make_buffer(vk::DeviceSize size,
	                                                                vk::BufferUsageFlags usage,
	                                                                vk::MemoryPropertyFlags properties);

	vk::raii::RenderPass renderpass{nullptr};
	vk::raii::PipelineLayout pipeline_layout{nullptr};
	vk::raii::Pipeline pipeline{nullptr};

	// Separable Gaussian blur (fullscreen triangle, sampled input).
	vk::raii::RenderPass blur_renderpass{nullptr};
	vk::raii::PipelineLayout blur_layout{nullptr};
	vk::raii::Pipeline blur_pipeline{nullptr};
	// Box-downsample pipeline (fullscreen triangle, shared layout: it only
	// reads the push block's src_texel prefix).
	vk::raii::Pipeline downsample_pipeline{nullptr};
	vk::raii::DescriptorSetLayout descriptor_layout{nullptr};
	vk::raii::DescriptorPool descriptor_pool{nullptr};
	// One set per (pass, eye): descriptor updates are host-side writes
	// that complete before submit, so every draw would otherwise read the
	// LAST update (all passes sampling the final image, i.e. unwritten
	// data). Distinct sets make each binding stable across the frame.
	// Index: 0,1 = blur-H eyes 0,1; 2,3 = blur-V eyes 0,1; 4..9 =
	// downsample levels 1..3 x eyes 0,1.
	std::vector<vk::raii::DescriptorSet> descriptor_sets;
	vk::raii::Sampler sampler{nullptr};

	// Blur intermediates (full mask resolution, own images: full usage
	// control, unlike swapchain images). Recreated when the extent changes.
	struct blur_target
	{
		vk::raii::Image image{nullptr};
		vk::raii::DeviceMemory memory{nullptr};
		std::vector<vk::raii::ImageView> views;
		std::vector<vk::raii::Framebuffer> raster_fbs;
		std::vector<vk::raii::Framebuffer> blur_fbs;
	};
	blur_target target_a;
	blur_target target_b;
	// Downsample chain (D) and tier blur workspaces (E) at half, quarter
	// and eighth mask resolution (index 0..2). Tier k blurs at 1/k size
	// with the same fixed kernel, so tap density (and cost) stays flat
	// while feather-px grows; the band stays continuous by exact mapping.
	blur_target down_targets[3];
	blur_target eblur_targets[3];
	vk::Extent2D targets_extent{0, 0};
	void ensure_targets(vk::Extent2D extent);
	void update_source(vk::ImageView view, uint32_t set);

	vk::raii::Buffer vertex_buffer{nullptr};
	vk::raii::DeviceMemory vertex_memory{nullptr};
	vk::raii::Buffer index_buffer{nullptr};
	vk::raii::DeviceMemory index_memory{nullptr};
	uint32_t index_count = 0;

	// Persistent staging for mesh uploads (resized on demand) + pending
	// copy applied at the start of the next record().
	vk::raii::Buffer staging_buffer{nullptr};
	vk::raii::DeviceMemory staging_memory{nullptr};
	vk::DeviceSize staging_size = 0;
	std::optional<std::pair<vk::DeviceSize, vk::DeviceSize>> pending_upload;

	// Framebuffers + views per swapchain image (images cycle; entries for
	// retired swapchains are dropped via reset_targets()).
	struct frame_targets
	{
		vk::Extent2D extent;
		std::array<vk::raii::ImageView, 2> views;
		std::array<vk::raii::Framebuffer, 2> framebuffers;
		frame_targets(vk::Extent2D extent_,
		              std::array<vk::raii::ImageView, 2> views_,
		              std::array<vk::raii::Framebuffer, 2> framebuffers_) :
		        extent(extent_),
		        views(std::move(views_)),
		        framebuffers(std::move(framebuffers_))
		{
		}
	};
	std::unordered_map<VkImage, frame_targets> targets;

	void flush_upload(vk::raii::CommandBuffer & cmd);
};
