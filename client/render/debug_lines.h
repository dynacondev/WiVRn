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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>
#include <vulkan/vulkan_raii.hpp>

#include "vk/allocation.h"

// World-space debug line renderer: 1px line segments (position + RGBA)
// drawn over the eye images with a per-eye MVP. Replaces the old tinted
// compositor quads (one layer per quad blew maxLayerCount): all debug
// geometry submits zero composition layers. Lines have no faces, so no
// facing math and no backface doubling. Blending is in the pipeline, so
// no FB extensions are needed either.
//
// Lifetime mirrors the video swapchain (rebuilt with it): per-(image,
// eye) framebuffers over the swapchain images, one host-visible vertex
// buffer grown on demand. Render thread only.
class debug_lines_renderer
{
public:
	struct vertex
	{
		float pos[3];
		float color[4];
	};

	debug_lines_renderer(vk::raii::Device & device, vk::Format format, std::vector<vk::Image> images,
	                     vk::Extent2D extent);

	// Record segments (vertex PAIRS) into both eye layers of images[index].
	// No-op (no barrier, no passes) when verts is empty. extents size the
	// per-eye viewports (the submitted layer rects); mvp maps world to NDC.
	void record(vk::raii::CommandBuffer & cmd, size_t image_index,
	            const std::array<vk::Extent2D, 2> & extents, const std::array<glm::mat4, 2> & mvp,
	            const vertex * verts, size_t vert_count);

private:
	vk::raii::Device * device = nullptr;

	vk::raii::PipelineLayout pipeline_layout{nullptr};
	vk::raii::RenderPass renderpass{nullptr};
	vk::raii::Pipeline pipeline{nullptr};

	struct target
	{
		vk::raii::ImageView view{nullptr};
		vk::raii::Framebuffer framebuffer{nullptr};
	};
	// [image][eye], parallel to the swapchain image list.
	std::vector<std::array<target, 2>> targets;
	std::vector<vk::Image> images;
	vk::Extent2D extent{0, 0};

	buffer_allocation staging;
	size_t staging_verts = 0;
};
