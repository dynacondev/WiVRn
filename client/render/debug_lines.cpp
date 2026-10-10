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

#include "render/debug_lines.h"

#include "vk/shader.h"

#include <cstddef>
#include <cstring>

#include <spdlog/spdlog.h>

namespace
{
struct push
{
	glm::mat4 mvp;
};
} // namespace

debug_lines_renderer::debug_lines_renderer(vk::raii::Device & device) : device(&device) {}

void debug_lines_renderer::ensure(xr::instance & inst, xr::session & sess, vk::Format format, int w, int h)
{
	if (ready() and format_ == format and (int)extent_.width == w and (int)extent_.height == h)
		return;
	// Rare path (first frame, video resize): nothing outstanding (we
	// acquire + release within one frame, always paired below).
	device->waitIdle();
	format_ = format;
	extent_ = {(uint32_t)w, (uint32_t)h};
	swapchain_ = xr::swapchain(inst, sess, *device, format, w, h, 1, 2);
	spdlog::info("Debug lines swapchain: {}x{} ({} images)", w, h, swapchain_.image_count());
	init_pipelines(format);
	targets.clear();
	for (vk::Image image: swapchain_.images())
	{
		targets.emplace_back();
		for (uint32_t eye = 0; eye < 2; ++eye)
		{
			vk::ImageViewCreateInfo iv_info{
			        .image = image,
			        .viewType = vk::ImageViewType::e2DArray,
			        .format = format,
			        .subresourceRange = {
			                .aspectMask = vk::ImageAspectFlagBits::eColor,
			                .baseMipLevel = 0,
			                .levelCount = 1,
			                .baseArrayLayer = eye,
			                .layerCount = 1,
			        },
			};
			targets.back()[eye].view = vk::raii::ImageView(*device, iv_info);
			vk::FramebufferCreateInfo fb_info{
			        .renderPass = *renderpass,
			        .width = extent_.width,
			        .height = extent_.height,
			        .layers = 1,
			};
			fb_info.setAttachments(*targets.back()[eye].view);
			targets.back()[eye].framebuffer = vk::raii::Framebuffer(*device, fb_info);
		}
	}
}

int debug_lines_renderer::acquire()
{
	int idx = swapchain_.acquire();
	if (not swapchain_.wait(100'000'000))
	{
		// Never park forever: release the untouched acquisition to keep
		// pairing and skip the frame (mirrors the mask groups).
		swapchain_.release();
		return -1;
	}
	return idx;
}

void debug_lines_renderer::release()
{
	swapchain_.release();
}

void debug_lines_renderer::begin_frame()
{
	staging_used = 0;
}

// Append-only upload shared by record paths (one upload serves all draws
// in a frame; draws reference their own ranges). Fixed generous cap:
// frame usage is hundreds of verts; growing mid-frame would orphan
// earlier draws. Returns first-vertex index, or UINT32_MAX when dropped.
static uint32_t upload_verts(vk::raii::Device & device, buffer_allocation & staging, const debug_lines_renderer::vertex * verts,
                             size_t vert_count, size_t & staging_used)
{
	static constexpr size_t kStagingCapVerts = 4096;
	if (staging_used + vert_count > kStagingCapVerts)
	{
		static bool warned = false;
		if (not warned)
		{
			warned = true;
			spdlog::warn("debug_lines: frame vertex overflow, dropping");
		}
		return UINT32_MAX;
	}
	if (not staging or staging.info().size < kStagingCapVerts * sizeof(debug_lines_renderer::vertex))
	{
		staging = buffer_allocation(
		        device,
		        vk::BufferCreateInfo{
		                .size = kStagingCapVerts * sizeof(debug_lines_renderer::vertex),
		                .usage = vk::BufferUsageFlagBits::eVertexBuffer,
		        },
		        VmaAllocationCreateInfo{
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "debug lines");
	}
	uint32_t first_vertex = (uint32_t)staging_used;
	std::memcpy((char *)staging.map() + first_vertex * sizeof(debug_lines_renderer::vertex), verts,
	            vert_count * sizeof(debug_lines_renderer::vertex));
	staging.unmap();
	staging_used += vert_count;
	return first_vertex;
}

void debug_lines_renderer::record(vk::raii::CommandBuffer & cmd, size_t image_index,
                                  const std::array<vk::Extent2D, 2> & extents,
                                  const std::array<glm::mat4, 2> & mvp, const vertex * verts, size_t vert_count,
                                  bool blended)
{
	if (vert_count == 0 or verts == nullptr or image_index >= targets.size())
		return;
	uint32_t first_vertex = upload_verts(*device, staging, verts, vert_count, staging_used);
	if (first_vertex == UINT32_MAX)
		return;

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blended ? *pipeline : *pipeline_unblended);
	cmd.bindVertexBuffers(0, vk::Buffer(staging), (vk::DeviceSize)0);

	// Fresh CLEAR images every frame: no barrier, no layout history.
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		auto & tgt = targets[image_index][eye];
		cmd.pushConstants<push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, push{mvp[eye]});
		cmd.setViewport(0, vk::Viewport{
		                        .x = 0,
		                        .y = 0,
		                        .width = (float)extents[eye].width,
		                        .height = (float)extents[eye].height,
		                        .minDepth = 0,
		                        .maxDepth = 1,
		                });
		cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extents[eye]});
		vk::ClearValue clear{};
		clear.color.float32.fill(0);
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *tgt.framebuffer,
		        .renderArea = {.offset = {0, 0}, .extent = extents[eye]},
		        .clearValueCount = 1,
		        .pClearValues = &clear,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		cmd.draw((uint32_t)vert_count, 1, first_vertex, 0);
		cmd.endRenderPass();
	}
}

void debug_lines_renderer::record_tris(vk::raii::CommandBuffer & cmd, size_t image_index,
                                       const std::array<vk::Extent2D, 2> & extents,
                                       const std::array<glm::mat4, 2> & mvp, const vertex * verts,
                                       size_t vert_count)
{
	if (vert_count == 0 or verts == nullptr or image_index >= targets.size())
		return;
	uint32_t first_vertex = upload_verts(*device, staging, verts, vert_count, staging_used);
	if (first_vertex == UINT32_MAX)
		return;

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_tri);
	cmd.bindVertexBuffers(0, vk::Buffer(staging), (vk::DeviceSize)0);

	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		auto & tgt = targets[image_index][eye];
		cmd.pushConstants<push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, push{mvp[eye]});
		cmd.setViewport(0, vk::Viewport{
		                        .x = 0,
		                        .y = 0,
		                        .width = (float)extents[eye].width,
		                        .height = (float)extents[eye].height,
		                        .minDepth = 0,
		                        .maxDepth = 1,
		                });
		cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extents[eye]});
		vk::ClearValue clear{};
		clear.color.float32.fill(0);
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *tgt.framebuffer,
		        .renderArea = {.offset = {0, 0}, .extent = extents[eye]},
		        .clearValueCount = 1,
		        .pClearValues = &clear,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		cmd.draw((uint32_t)vert_count, 1, first_vertex, 0);
		cmd.endRenderPass();
	}
}

void debug_lines_renderer::init_pipelines(vk::Format format)
{
	auto vert = load_shader(*device, "gizmo.vert");
	auto frag = load_shader(*device, "gizmo.frag");

	vk::PipelineShaderStageCreateInfo stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **frag,
	                .pName = "main",
	        },
	};

	vk::VertexInputBindingDescription binding{
	        .binding = 0,
	        .stride = sizeof(vertex),
	        .inputRate = vk::VertexInputRate::eVertex,
	};
	vk::VertexInputAttributeDescription attributes[2] = {
	        {
	                .location = 0,
	                .binding = 0,
	                .format = vk::Format::eR32G32B32Sfloat,
	                .offset = offsetof(vertex, pos),
	        },
	        {
	                .location = 1,
	                .binding = 0,
	                .format = vk::Format::eR32G32B32A32Sfloat,
	                .offset = offsetof(vertex, color),
	        },
	};
	vk::PipelineVertexInputStateCreateInfo vertex_input{
	        .vertexBindingDescriptionCount = 1,
	        .pVertexBindingDescriptions = &binding,
	        .vertexAttributeDescriptionCount = 2,
	        .pVertexAttributeDescriptions = attributes,
	};

	vk::PipelineInputAssemblyStateCreateInfo input_assembly{
	        .topology = vk::PrimitiveTopology::eLineList,
	};

	vk::PipelineViewportStateCreateInfo viewport_state{
	        .viewportCount = 1,
	        .scissorCount = 1,
	};

	vk::PipelineRasterizationStateCreateInfo rasterization{
	        .polygonMode = vk::PolygonMode::eFill,
	        .cullMode = vk::CullModeFlagBits::eNone,
	        .lineWidth = 1,
	};

	vk::PipelineMultisampleStateCreateInfo multisample{
	        .rasterizationSamples = vk::SampleCountFlagBits::e1,
	};

	// Straight alpha blend over transparent (fade lives in vertex alpha).
	vk::PipelineColorBlendAttachmentState blend_attachment{
	        .blendEnable = VK_TRUE,
	        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
	        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
	        .colorBlendOp = vk::BlendOp::eAdd,
	        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
	        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
	        .alphaBlendOp = vk::BlendOp::eAdd,
	        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	};
	vk::PipelineColorBlendStateCreateInfo blend{
	        .attachmentCount = 1,
	        .pAttachments = &blend_attachment,
	};

	vk::DynamicState dynamics[2] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic{
	        .dynamicStateCount = 2,
	        .pDynamicStates = dynamics,
	};

	vk::PushConstantRange push_range{
	        .stageFlags = vk::ShaderStageFlagBits::eVertex,
	        .offset = 0,
	        .size = sizeof(push),
	};
	vk::PipelineLayoutCreateInfo layout_info{
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &push_range,
	};
	pipeline_layout = vk::raii::PipelineLayout(*device, layout_info);

	// CLEAR on UNDEFINED every frame (mirrors the raster passes): no
	// layout history is ever assumed. Final GENERAL: universally
	// samplable at submit (mask precedent).
	vk::AttachmentDescription attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eClear,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &color_ref,
	};
	vk::RenderPassCreateInfo renderpass_info{
	        .attachmentCount = 1,
	        .pAttachments = &attachment,
	        .subpassCount = 1,
	        .pSubpasses = &subpass,
	};
	renderpass = vk::raii::RenderPass(*device, renderpass_info);

	vk::GraphicsPipelineCreateInfo pipeline_info{
	        .stageCount = 2,
	        .pStages = stages,
	        .pVertexInputState = &vertex_input,
	        .pInputAssemblyState = &input_assembly,
	        .pViewportState = &viewport_state,
	        .pRasterizationState = &rasterization,
	        .pMultisampleState = &multisample,
	        .pColorBlendState = &blend,
	        .pDynamicState = &dynamic,
	        .layout = *pipeline_layout,
	        .renderPass = *renderpass,
	};
	pipeline = vk::raii::Pipeline(*device, nullptr, pipeline_info);

	// TEMP diagnostic twin: identical except blending off (isolates blend
	// faults from pipeline/framebuffer faults).
	vk::PipelineColorBlendAttachmentState blend_off_attachment = blend_attachment;
	blend_off_attachment.blendEnable = VK_FALSE;
	vk::PipelineColorBlendStateCreateInfo blend_off{
	        .attachmentCount = 1,
	        .pAttachments = &blend_off_attachment,
	};
	vk::GraphicsPipelineCreateInfo pipeline_off_info = pipeline_info;
	pipeline_off_info.pColorBlendState = &blend_off;
	pipeline_unblended = vk::raii::Pipeline(*device, nullptr, pipeline_off_info);

	// TEMP diagnostic twin: triangle list, unblended (topology bisect:
	// tri-visible + lines-invisible convicts line rasterization).
	vk::PipelineInputAssemblyStateCreateInfo tri_assembly = input_assembly;
	tri_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::GraphicsPipelineCreateInfo pipeline_tri_info = pipeline_off_info;
	pipeline_tri_info.pInputAssemblyState = &tri_assembly;
	pipeline_tri = vk::raii::Pipeline(*device, nullptr, pipeline_tri_info);
}
