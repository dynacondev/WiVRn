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

// Feather-masks scaffolding: one mask group renders a coverage image the
// camera layers consume through DST_ALPHA (mask 1 = reality). This branch
// carries the shell only: clear + debug-cutout punch. Silhouette raster,
// blur chain and per-object grouping land with objects; until then the
// only group is the debug one and the image carries cutout holes (alpha
// 1) on clear (alpha 0), so the flat camera shows reality exactly at
// sighted QR windows and game video elsewhere.
//
// Rendered with a dedicated minimal pipeline (no descriptors, MVP via push
// constants, no depth, no culling). Swapchain images only allow
// COLOR_ATTACHMENT output, which is why this is a raster pass and not a
// buffer copy.

#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <openxr/openxr.h>
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

	// Record clear + cutout punch for both eyes into the layers of an
	// acquired swapchain image. The image must be unused (UNDEFINED is
	// fine); it is left in GENERAL for the compositor. Cutouts are
	// world-space quads (two triangles each) punched hard at full alpha
	// (mask 1 = reality), sharing one view-only MVP. No-op when cutouts
	// is empty (image still cleared: a group only submits when active).
	void record(vk::raii::CommandBuffer & cmd,
	            vk::Image image,
	            vk::Extent2D extent,
	            const std::vector<std::array<glm::vec3, 6>> & cutouts,
	            const std::array<glm::mat4, 2> & cutout_mvp);

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

	vk::raii::RenderPass renderpass{nullptr};
	vk::raii::PipelineLayout pipeline_layout{nullptr};
	vk::raii::Pipeline pipeline{nullptr};

	// Host-visible cutout quad buffer (worst case: every configured
	// marker in view at once). Reused across frames; bounds checked.
	vk::raii::Buffer cutout_buffer{nullptr};
	vk::raii::DeviceMemory cutout_memory{nullptr};
	vk::DeviceSize cutout_capacity = 0;

	// Framebuffers + views per swapchain image (images cycle; entries for
	// retired extents are dropped via reset_targets()).
	struct frame_targets
	{
		vk::Extent2D extent;
		std::array<vk::raii::ImageView, 2> views;
		std::array<vk::raii::Framebuffer, 2> framebuffers;
	};
	std::unordered_map<VkImage, frame_targets> targets;

	void ensure_cutout_buffer(vk::DeviceSize size);
};
