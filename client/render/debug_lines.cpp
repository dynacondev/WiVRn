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

namespace
{
struct push
{
	glm::mat4 mvp;
};
} // namespace

debug_lines_renderer::debug_lines_renderer(vk::raii::Device & device, vk::Format format,
                                           std::vector<vk::Image> images, vk::Extent2D extent) :
        device(&device),
        images(std::move(images)),
        extent(extent)
{
	auto vert = load_shader(device, "gizmo.vert");
	auto frag = load_shader(device, "gizmo.frag");

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

	// Straight alpha blend over video (fade lives in vertex alpha).
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
	pipeline_layout = vk::raii::PipelineLayout(device, layout_info);

	// LOAD/STORE on GENERAL: the eye image already holds video (defoveate
	// left it COLOR_ATTACHMENT_OPTIMAL; both are valid color-attachment
	// layouts, the barrier below orders the passes).
	vk::AttachmentDescription attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eLoad,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
	        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
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
	renderpass = vk::raii::RenderPass(device, renderpass_info);

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
	pipeline = vk::raii::Pipeline(device, nullptr, pipeline_info);

	// TEMP diagnostic twin: identical except blending off (isolates blend
	// faults from pipeline/framebuffer/barrier faults).
	vk::PipelineColorBlendAttachmentState blend_off_attachment = blend_attachment;
	blend_off_attachment.blendEnable = VK_FALSE;
	vk::PipelineColorBlendStateCreateInfo blend_off{
	        .attachmentCount = 1,
	        .pAttachments = &blend_off_attachment,
	};
	vk::GraphicsPipelineCreateInfo pipeline_off_info = pipeline_info;
	pipeline_off_info.pColorBlendState = &blend_off;
	pipeline_unblended = vk::raii::Pipeline(device, nullptr, pipeline_off_info);

	// TEMP diagnostic twin: triangle list, unblended (topology bisect:
	// tri-visible + lines-invisible convicts line rasterization).
	vk::PipelineInputAssemblyStateCreateInfo tri_assembly = input_assembly;
	tri_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::GraphicsPipelineCreateInfo pipeline_tri_info = pipeline_off_info;
	pipeline_tri_info.pInputAssemblyState = &tri_assembly;
	pipeline_tri = vk::raii::Pipeline(device, nullptr, pipeline_tri_info);

	// Per-(image, eye) framebuffers (one layer each, like the defoveator).
	for (vk::Image image: images)
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
			targets.back()[eye].view = vk::raii::ImageView(device, iv_info);
			vk::FramebufferCreateInfo fb_info{
			        .renderPass = *renderpass,
			        .width = extent.width,
			        .height = extent.height,
			        .layers = 1,
			};
			fb_info.setAttachments(*targets.back()[eye].view);
			targets.back()[eye].framebuffer = vk::raii::Framebuffer(device, fb_info);
		}
	}
}

void debug_lines_renderer::record(vk::raii::CommandBuffer & cmd, size_t image_index,
                                  const std::array<vk::Extent2D, 2> & extents,
                                  const std::array<glm::mat4, 2> & mvp, const vertex * verts, size_t vert_count,
                                  bool blended)
{
	if (vert_count == 0 or verts == nullptr or image_index >= targets.size())
		return;

	size_t need = vert_count * sizeof(vertex);
	if (not staging or staging.info().size < need)
	{
		staging = buffer_allocation(
		        *device,
		        vk::BufferCreateInfo{
		                .size = need,
		                .usage = vk::BufferUsageFlagBits::eVertexBuffer,
		        },
		        VmaAllocationCreateInfo{
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "debug lines");
	}
	std::memcpy(staging.map(), verts, need);
	staging.unmap();

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blended ? *pipeline : *pipeline_unblended);
	cmd.bindVertexBuffers(0, vk::Buffer(staging), (vk::DeviceSize)0);

	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		auto & tgt = targets[image_index][eye];
		// Same-queue dependency on the defoveate writes above (cf. the
		// cutout barrier): separate render passes need it spelled out.
		// Images always arrive via defoveate (OPTIMAL), so no transition.
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    {},
		                    {},
		                    {},
		                    vk::ImageMemoryBarrier{
		                            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		                            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
		                                             vk::AccessFlagBits::eColorAttachmentWrite,
		                            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		                            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
		                            .image = images[image_index],
		                            .subresourceRange = {
		                                    .aspectMask = vk::ImageAspectFlagBits::eColor,
		                                    .levelCount = 1,
		                                    .baseArrayLayer = eye,
		                                    .layerCount = 1,
		                            },
		                    });
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
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *tgt.framebuffer,
		        .renderArea = {.offset = {0, 0}, .extent = extents[eye]},
		        .clearValueCount = 0,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		cmd.draw((uint32_t)vert_count, 1, 0, 0);
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

	size_t need = vert_count * sizeof(vertex);
	if (not staging or staging.info().size < need)
	{
		staging = buffer_allocation(
		        *device,
		        vk::BufferCreateInfo{
		                .size = need,
		                .usage = vk::BufferUsageFlagBits::eVertexBuffer,
		        },
		        VmaAllocationCreateInfo{
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "debug lines");
	}
	std::memcpy(staging.map(), verts, need);
	staging.unmap();

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_tri);
	cmd.bindVertexBuffers(0, vk::Buffer(staging), (vk::DeviceSize)0);

	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		auto & tgt = targets[image_index][eye];
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    {},
		                    {},
		                    {},
		                    vk::ImageMemoryBarrier{
		                            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		                            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
		                                             vk::AccessFlagBits::eColorAttachmentWrite,
		                            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		                            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
		                            .image = images[image_index],
		                            .subresourceRange = {
		                                    .aspectMask = vk::ImageAspectFlagBits::eColor,
		                                    .levelCount = 1,
		                                    .baseArrayLayer = eye,
		                                    .layerCount = 1,
		                            },
		                    });
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
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *tgt.framebuffer,
		        .renderArea = {.offset = {0, 0}, .extent = extents[eye]},
		        .clearValueCount = 0,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		cmd.draw((uint32_t)vert_count, 1, 0, 0);
		cmd.endRenderPass();
	}
}
