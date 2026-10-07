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

// Feathered passthrough window mask: rasterizes the calibrated mesh
// silhouette (flat white, alpha 1) into a tiny per-eye target. The
// compositor's bilinear upscale turns the binary raster into the
// alpha-gradient feather band, so no blur pass is needed: feather width
// in screen pixels ~= screen width / mask width.
//
// Rendered with a dedicated minimal pipeline (no descriptors, MVP via push
// constants, no depth, no culling so concave self-overlap still unions).
// Swapchain images only allow COLOR_ATTACHMENT output, which is why this is
// a raster pass and not a buffer copy.

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

	// Record both eye passes into the layers of an acquired swapchain image.
	// No-op when no mesh is set. The image must be unused (UNDEFINED is
	// fine); it is left in GENERAL for the compositor.
	void record(vk::raii::CommandBuffer & cmd,
	            vk::Image image,
	            vk::Extent2D extent,
	            const std::array<glm::mat4, 2> & mvp);

	bool has_mesh() const
	{
		return index_count > 0;
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

	// Framebuffers + views per swapchain image (images cycle, sizes change
	// with the feather setting: entries for retired extents are dropped).
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
