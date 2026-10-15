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

// Feather-mask shell: clear + debug-cutout punch. See feather_mask.h.

#include "render/feather_mask.h"

#include "vk/shader.h"

#include <cstring>
#include <stdexcept>

namespace
{
// Raster push block. Mirrors mask.vert/frag.glsl PushConstants.
struct raster_push
{
	glm::mat4 mvp;
	float opacity = 1;
};

uint32_t find_memory_type(vk::raii::PhysicalDevice & physical_device, uint32_t type_bits, vk::MemoryPropertyFlags properties)
{
	auto props = physical_device.getMemoryProperties();
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
	{
		auto & mem = props.memoryTypes[i];
		if ((type_bits & (1u << i)) and (mem.propertyFlags & properties) == properties)
			return i;
	}
	throw std::runtime_error("feather_mask: no suitable memory type");
}
} // namespace

feather_mask_renderer::feather_mask_renderer(vk::raii::Device & device_,
                                             vk::raii::PhysicalDevice & physical_device_,
                                             vk::Format format_) :
        device(device_),
        physical_device(physical_device_),
        format(format_)
{
	auto vert = load_shader(device, "mask.vert");
	auto frag = load_shader(device, "mask.frag");

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
	        .stride = sizeof(glm::vec3),
	        .inputRate = vk::VertexInputRate::eVertex,
	};
	vk::VertexInputAttributeDescription attribute{
	        .location = 0,
	        .binding = 0,
	        .format = vk::Format::eR32G32B32Sfloat,
	        .offset = 0,
	};
	vk::PipelineVertexInputStateCreateInfo vertex_input{
	        .vertexBindingDescriptionCount = 1,
	        .pVertexBindingDescriptions = &binding,
	        .vertexAttributeDescriptionCount = 1,
	        .pVertexAttributeDescriptions = &attribute,
	};

	vk::PipelineInputAssemblyStateCreateInfo input_assembly{
	        .topology = vk::PrimitiveTopology::eTriangleList,
	};

	vk::PipelineViewportStateCreateInfo viewport_state{
	        .viewportCount = 1,
	        .scissorCount = 1,
	};

	vk::PipelineRasterizationStateCreateInfo rasterization{
	        .polygonMode = vk::PolygonMode::eFill,
	        .cullMode = vk::CullModeFlagBits::eNone,
	        .frontFace = vk::FrontFace::eCounterClockwise,
	        .lineWidth = 1,
	};

	vk::PipelineMultisampleStateCreateInfo multisample{
	        .rasterizationSamples = vk::SampleCountFlagBits::e1,
	};

	vk::PipelineColorBlendAttachmentState blend_attachment{
	        .blendEnable = VK_FALSE,
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
	        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
	        .offset = 0,
	        .size = sizeof(raster_push),
	};
	vk::PipelineLayoutCreateInfo layout_info{
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &push_range,
	};
	pipeline_layout = vk::raii::PipelineLayout(device, layout_info);

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
	renderpass = vk::raii::RenderPass(device, renderpass_info);
}

void feather_mask_renderer::ensure_cutout_buffer(vk::DeviceSize size)
{
	if (cutout_capacity >= size)
		return;
	cutout_buffer = vk::raii::Buffer{nullptr};
	cutout_memory = vk::raii::DeviceMemory{nullptr};
	vk::BufferCreateInfo buffer_info{
	        .size = size,
	        .usage = vk::BufferUsageFlagBits::eVertexBuffer,
	        .sharingMode = vk::SharingMode::eExclusive,
	};
	cutout_buffer = vk::raii::Buffer(device, buffer_info);
	auto requirements = cutout_buffer.getMemoryRequirements();
	vk::MemoryAllocateInfo alloc_info{
	        .allocationSize = requirements.size,
	        .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits,
	                                            vk::MemoryPropertyFlagBits::eHostVisible |
	                                                    vk::MemoryPropertyFlagBits::eHostCoherent),
	};
	cutout_memory = vk::raii::DeviceMemory(device, alloc_info);
	cutout_buffer.bindMemory(*cutout_memory, 0);
	cutout_capacity = size;
}

void feather_mask_renderer::record(vk::raii::CommandBuffer & cmd,
                                   vk::Image image,
                                   vk::Extent2D extent,
                                   const std::vector<std::array<glm::vec3, 6>> & cutouts,
                                   const std::array<glm::mat4, 2> & cutout_mvp)
{
	auto it = targets.find(image);
	if (it == targets.end() or it->second.extent.width != extent.width or it->second.extent.height != extent.height)
	{
		if (it != targets.end())
			targets.erase(it);
		// NOTE: views below are named locals, not &* temporaries: the
		// framebuffer create-info stores the pointer, so the VkImageView
		// must outlive the vk::raii::Framebuffer constructor call.
		// (&* on a vk::raii handle takes the address of a returned
		// temporary: freed stack read as an image view, GPU fault, silent
		// fence hang. That exact bug froze the first mask frame.)
		std::array<vk::raii::ImageView, 2> views{{
		        [&] {
			        vk::ImageViewCreateInfo view_info{
			                .image = image,
			                .viewType = vk::ImageViewType::e2D,
			                .format = format,
			                .subresourceRange = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseMipLevel = 0,
			                        .levelCount = 1,
			                        .baseArrayLayer = 0,
			                        .layerCount = 1,
			                },
			        };
			        return vk::raii::ImageView(device, view_info);
		        }(),
		        [&] {
			        vk::ImageViewCreateInfo view_info{
			                .image = image,
			                .viewType = vk::ImageViewType::e2D,
			                .format = format,
			                .subresourceRange = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseMipLevel = 0,
			                        .levelCount = 1,
			                        .baseArrayLayer = 1,
			                        .layerCount = 1,
			                },
			        };
			        return vk::raii::ImageView(device, view_info);
		        }(),
		}};
		vk::ImageView raw_view_0 = *views[0];
		vk::ImageView raw_view_1 = *views[1];
		std::array<vk::raii::Framebuffer, 2> framebuffers{{
		        [&] {
			        vk::FramebufferCreateInfo fb_info{
			                .renderPass = *renderpass,
			                .width = extent.width,
			                .height = extent.height,
			                .layers = 1,
			        };
			        fb_info.setAttachments(raw_view_0);
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		        [&] {
			        vk::FramebufferCreateInfo fb_info{
			                .renderPass = *renderpass,
			                .width = extent.width,
			                .height = extent.height,
			                .layers = 1,
			        };
			        fb_info.setAttachments(raw_view_1);
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		}};
		it = targets.emplace(image, frame_targets{extent, std::move(views), std::move(framebuffers)}).first;
	}

	// Cutout quads land last at full alpha (mask 1 = reality): with no
	// silhouette raster yet, the cleared image carries exactly the debug
	// windows. Blend is off: the punch overwrites.
	ensure_cutout_buffer(cutouts.size() * 6 * sizeof(glm::vec3));
	if (not cutouts.empty())
	{
		void * dst = cutout_memory.mapMemory(0, cutouts.size() * 6 * sizeof(glm::vec3));
		std::memcpy(dst, cutouts.data(), cutouts.size() * 6 * sizeof(glm::vec3));
		cutout_memory.unmapMemory();
	}

	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		cmd.setViewport(0, vk::Viewport{
		                        .x = 0,
		                        .y = 0,
		                        .width = (float)extent.width,
		                        .height = (float)extent.height,
		                        .minDepth = 0,
		                        .maxDepth = 1,
		                });
		cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});
		vk::ClearValue clear;
		clear.color = vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 0});
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *renderpass,
		        .framebuffer = *it->second.framebuffers[eye],
		        .renderArea = {{0, 0}, extent},
		        .clearValueCount = 1,
		        .pClearValues = &clear,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
		if (not cutouts.empty())
		{
			vk::DeviceSize offset = 0;
			cmd.bindVertexBuffers(0, *cutout_buffer, offset);
			cmd.pushConstants<raster_push>(*pipeline_layout,
			                               vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
			                               raster_push{.mvp = cutout_mvp[eye], .opacity = 1});
			cmd.draw((uint32_t)(cutouts.size() * 6), 1, 0, 0);
		}
		cmd.endRenderPass();
	}
}
