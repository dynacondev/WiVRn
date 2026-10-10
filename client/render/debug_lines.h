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
#include "xr/swapchain.h"

// World-space debug line renderer: 1px line segments (position + RGBA)
// composited as ONE transparent projection layer (standard alpha blend,
// no FB extensions needed). One layer always: no per-quad layers, so
// maxLayerCount can never overflow no matter the tag count. Lines have
// no faces, so no facing math and no backface doubling. Blending is in
// the pipeline.
//
// Own swapchain, CLEAR every frame (UNDEFINED initial: no layout history
// is ever assumed — the eye-image LOAD variant of this class died on
// exactly that assumption). Per-(image, eye) framebuffers, one
// host-visible vertex buffer grown on demand. Render thread only.
namespace xr
{
class instance;
class session;
} // namespace xr

class debug_lines_renderer
{
public:
	struct vertex
	{
		float pos[3];
		float color[4];
	};

	explicit debug_lines_renderer(vk::raii::Device & device);

	// (Re)create swapchain + targets + pipelines for format/size.
	// waitIdle inside on rebuild; no-op when unchanged.
	void ensure(xr::instance & inst, xr::session & sess, vk::Format format, int w, int h);

	bool ready() const
	{
		return (bool)swapchain_;
	}

	// Acquire + wait (100ms); -1 on timeout (pairing-safe: the untouched
	// acquisition is released inside, skip the frame).
	int acquire();

	// Record segments (vertex PAIRS) into both eye layers of the acquired
	// image. No-op when verts is empty. extents size the per-eye
	// viewports (the submitted layer rects); mvp maps world to NDC.
	// blended=false selects the blend-off pipeline (TEMP diagnostic: skips
	// the alpha path to isolate blending faults).
	void record(vk::raii::CommandBuffer & cmd, size_t image_index,
	            const std::array<vk::Extent2D, 2> & extents, const std::array<glm::mat4, 2> & mvp,
	            const vertex * verts, size_t vert_count, bool blended = true);
	void record_tris(vk::raii::CommandBuffer & cmd, size_t image_index,
	                 const std::array<vk::Extent2D, 2> & extents, const std::array<glm::mat4, 2> & mvp,
	                 const vertex * verts, size_t vert_count);

	// TEMP diagnostic twin: triangle list through the same renderpass /
	// framebuffers / vertex buffer, unblended. Tri-visible vs
	// lines-invisible bisects primitive-topology faults from shared-infra
	// faults. Revert with the line-visibility verdict.

	// Reset the append cursor (once per frame, before records). All
	// records in a frame share one upload; without the cursor, later
	// memcpys clobber earlier draws' data (same buffer, draw at submit).
	void begin_frame();

	// Release the acquired image (after submit, before endFrame — the
	// compositor takes the last released image).
	void release();

	xr::swapchain & swapchain()
	{
		return swapchain_;
	}

private:
	void init_pipelines(vk::Format format);
	void init_targets();

	vk::raii::Device * device = nullptr;

	vk::raii::PipelineLayout pipeline_layout{nullptr};
	vk::raii::RenderPass renderpass{nullptr};
	vk::raii::Pipeline pipeline{nullptr};
	vk::raii::Pipeline pipeline_unblended{nullptr}; // TEMP diagnostic: blend-off variant
	vk::raii::Pipeline pipeline_tri{nullptr}; // TEMP diagnostic: triangle twin (topology bisect)

	xr::swapchain swapchain_;
	vk::Format format_ = vk::Format::eUndefined;
	vk::Extent2D extent_{0, 0};

	struct target
	{
		vk::raii::ImageView view{nullptr};
		vk::raii::Framebuffer framebuffer{nullptr};
	};
	// [image][eye], parallel to the swapchain image list.
	std::vector<std::array<target, 2>> targets;

	buffer_allocation staging;
	size_t staging_used = 0; // verts consumed this frame (reset by begin_frame)
};
