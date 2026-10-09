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

#include "render/feather_mask.h"

#include "vk/shader.h"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace
{
uint32_t find_memory_type(vk::raii::PhysicalDevice & physical_device, uint32_t type_bits, vk::MemoryPropertyFlags properties)
{
	auto mem = physical_device.getMemoryProperties();
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
	{
		if ((type_bits & (1u << i)) and (mem.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	throw std::runtime_error("feather_mask: no suitable memory type");
}

struct blur_push
{
	float texel[2];
	float dir[2];
	float spread;
	float pad = 0;
};

// Silhouette raster push block. Mirrors mask.vert/frag.glsl PushConstants:
// mvp plus the first-acquisition fade opacity. The blur chain is linear in
// alpha, so one raster-alpha scale fades interior and feather band alike.
struct raster_push
{
	glm::mat4 mvp;
	float opacity = 1;
};

// Downsample level extent (level 1 = half): exact halving, so the 2x2 box
// mapping stays exact. Shared with ensure_targets: sizes must match.
// The mask extent is 64-quantized upstream, hence divisible throughout.
vk::Extent2D level_extent(vk::Extent2D full, int level)
{
	uint32_t w = full.width;
	uint32_t h = full.height;
	for (int i = 0; i < level; ++i)
	{
		w = std::max(8u, w / 2);
		h = std::max(8u, h / 2);
	}
	return {w, h};
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
	        .stride = sizeof(XrVector3f),
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

	// Cutout pass: same shape as the raster pass but LOAD, so the
	// finished (blurred) swapchain image can be re-begun and stamped.
	// Framebuffer-compatible with both passes above by construction.
	vk::AttachmentDescription cutout_attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eLoad,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eGeneral,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference cutout_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription cutout_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &cutout_ref,
	};
	vk::RenderPassCreateInfo cutout_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &cutout_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &cutout_subpass,
	};
	cutout_renderpass = vk::raii::RenderPass(device, cutout_rp_info);

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

	// Blur render pass: fully overwritten every use (fullscreen triangle),
	// so no clear and no prior contents needed.
	vk::AttachmentDescription blur_attachment{
	        .format = format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eDontCare,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference blur_color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription blur_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &blur_color_ref,
	};
	vk::RenderPassCreateInfo blur_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &blur_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &blur_subpass,
	};
	blur_renderpass = vk::raii::RenderPass(device, blur_rp_info);

	auto blur_vert = load_shader(device, "blur.vert");
	auto blur_frag = load_shader(device, "blur.frag");
	vk::PipelineShaderStageCreateInfo blur_stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **blur_vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **blur_frag,
	                .pName = "main",
	        },
	};

	vk::PipelineVertexInputStateCreateInfo blur_vertex_input{};
	vk::PipelineInputAssemblyStateCreateInfo blur_input_assembly{
	        .topology = vk::PrimitiveTopology::eTriangleList,
	};
	vk::PipelineViewportStateCreateInfo blur_viewport_state{
	        .viewportCount = 1,
	        .scissorCount = 1,
	};
	vk::PipelineRasterizationStateCreateInfo blur_rasterization{
	        .polygonMode = vk::PolygonMode::eFill,
	        .cullMode = vk::CullModeFlagBits::eNone,
	        .frontFace = vk::FrontFace::eCounterClockwise,
	        .lineWidth = 1,
	};
	vk::PipelineMultisampleStateCreateInfo blur_multisample{
	        .rasterizationSamples = vk::SampleCountFlagBits::e1,
	};
	vk::PipelineColorBlendAttachmentState blur_blend_attachment{
	        .blendEnable = VK_FALSE,
	        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	};
	vk::PipelineColorBlendStateCreateInfo blur_blend{
	        .attachmentCount = 1,
	        .pAttachments = &blur_blend_attachment,
	};
	vk::DynamicState blur_dynamics[2] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo blur_dynamic{
	        .dynamicStateCount = 2,
	        .pDynamicStates = blur_dynamics,
	};

	vk::DescriptorSetLayoutBinding sampler_binding{
	        .binding = 0,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 1,
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	};
	vk::DescriptorSetLayoutCreateInfo set_layout_info{
	        .bindingCount = 1,
	        .pBindings = &sampler_binding,
	};
	descriptor_layout = vk::raii::DescriptorSetLayout(device, set_layout_info);

	vk::PushConstantRange blur_push_range{
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        .offset = 0,
	        .size = 24,
	};
	vk::PipelineLayoutCreateInfo blur_layout_info{
	        .setLayoutCount = 1,
	        .pSetLayouts = &*descriptor_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &blur_push_range,
	};
	blur_layout = vk::raii::PipelineLayout(device, blur_layout_info);

	vk::GraphicsPipelineCreateInfo blur_pipeline_info{
	        .stageCount = 2,
	        .pStages = blur_stages,
	        .pVertexInputState = &blur_vertex_input,
	        .pInputAssemblyState = &blur_input_assembly,
	        .pViewportState = &blur_viewport_state,
	        .pRasterizationState = &blur_rasterization,
	        .pMultisampleState = &blur_multisample,
	        .pColorBlendState = &blur_blend,
	        .pDynamicState = &blur_dynamic,
	        .layout = *blur_layout,
	        .renderPass = *blur_renderpass,
	};
	blur_pipeline = vk::raii::Pipeline(device, nullptr, blur_pipeline_info);

	// Box-downsample pipeline: same fullscreen vertex shader, same layout
	// (reads only the push prefix) and render pass as the blur passes.
	auto downsample_frag = load_shader(device, "downsample.frag");
	vk::PipelineShaderStageCreateInfo downsample_stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **blur_vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **downsample_frag,
	                .pName = "main",
	        },
	};
	vk::PipelineVertexInputStateCreateInfo downsample_vertex_input{};
	vk::GraphicsPipelineCreateInfo downsample_pipeline_info{
	        .stageCount = 2,
	        .pStages = downsample_stages,
	        .pVertexInputState = &downsample_vertex_input,
	        .pInputAssemblyState = &blur_input_assembly,
	        .pViewportState = &blur_viewport_state,
	        .pRasterizationState = &blur_rasterization,
	        .pMultisampleState = &blur_multisample,
	        .pColorBlendState = &blur_blend,
	        .pDynamicState = &blur_dynamic,
	        .layout = *blur_layout,
	        .renderPass = *blur_renderpass,
	};
	downsample_pipeline = vk::raii::Pipeline(device, nullptr, downsample_pipeline_info);

	vk::SamplerCreateInfo sampler_info{
	        .magFilter = vk::Filter::eLinear,
	        .minFilter = vk::Filter::eLinear,
	        .mipmapMode = vk::SamplerMipmapMode::eLinear,
	        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	        .mipLodBias = 0,
	        .maxAnisotropy = 1,
	        .compareEnable = VK_FALSE,
	        .minLod = 0,
	        .maxLod = 0,
	};
	sampler = vk::raii::Sampler(device, sampler_info);

	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 10,
	};
	vk::DescriptorPoolCreateInfo pool_info{
	        .maxSets = 10,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	};
	descriptor_pool = vk::raii::DescriptorPool(device, pool_info);
	for (int i = 0; i < 10; ++i)
	{
		vk::DescriptorSetAllocateInfo set_alloc_info{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = 1,
		        .pSetLayouts = &*descriptor_layout,
		};
		descriptor_sets.emplace_back(std::move(device.allocateDescriptorSets(set_alloc_info)[0]));
	}
}

std::pair<vk::raii::Buffer, vk::raii::DeviceMemory> feather_mask_renderer::make_buffer(vk::DeviceSize size,
                                                                                       vk::BufferUsageFlags usage,
                                                                                       vk::MemoryPropertyFlags properties)
{
	vk::BufferCreateInfo buffer_info{
	        .size = size,
	        .usage = usage,
	        .sharingMode = vk::SharingMode::eExclusive,
	};
	vk::raii::Buffer buffer(device, buffer_info);
	auto requirements = buffer.getMemoryRequirements();
	vk::MemoryAllocateInfo alloc_info{
	        .allocationSize = requirements.size,
	        .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, properties),
	};
	vk::raii::DeviceMemory memory(device, alloc_info);
	buffer.bindMemory(*memory, 0);
	return {std::move(buffer), std::move(memory)};
}

void feather_mask_renderer::set_mesh(const std::string & key, const passthrough_mesh::triangle_soup & soup)
{
	if (soup.indices.empty() or soup.vertices.empty())
	{
		remove_mesh(key);
		return;
	}

	// Rare path (map change): make sure no in-flight frame still reads the
	// old buffers before replacing them.
	device.waitIdle();

	mesh_buffers & mesh = meshes[key];
	vk::DeviceSize vertex_bytes = soup.vertices.size() * sizeof(XrVector3f);
	vk::DeviceSize index_bytes = soup.indices.size() * sizeof(uint32_t);
	vk::DeviceSize total = vertex_bytes + index_bytes;

	if (mesh.staging_size < total)
	{
		std::tie(mesh.staging_buffer, mesh.staging_memory) = make_buffer(
		        total,
		        vk::BufferUsageFlagBits::eTransferSrc,
		        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		mesh.staging_size = total;
	}
	void * mapped = mesh.staging_memory.mapMemory(0, total);
	std::memcpy(mapped, soup.vertices.data(), vertex_bytes);
	std::memcpy((char *)mapped + vertex_bytes, soup.indices.data(), index_bytes);
	mesh.staging_memory.unmapMemory();

	std::tie(mesh.vertex_buffer, mesh.vertex_memory) = make_buffer(
	        vertex_bytes,
	        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
	        vk::MemoryPropertyFlagBits::eDeviceLocal);
	std::tie(mesh.index_buffer, mesh.index_memory) = make_buffer(
	        index_bytes,
	        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
	        vk::MemoryPropertyFlagBits::eDeviceLocal);

	mesh.pending_upload = {vertex_bytes, index_bytes};
	mesh.index_count = (uint32_t)soup.indices.size();
}

void feather_mask_renderer::remove_mesh(const std::string & key)
{
	auto it = meshes.find(key);
	if (it == meshes.end())
		return;
	// Same in-flight hazard as replace.
	device.waitIdle();
	meshes.erase(it);
}

void feather_mask_renderer::ensure_targets(vk::Extent2D extent)
{
	if (targets_extent.width == extent.width and targets_extent.height == extent.height and
	    targets_extent.width != 0)
		return;

	// Rare path (extent change): in-flight frames may still read the old
	// images, mirroring setup_reprojection_swapchain.
	device.waitIdle();
	targets_extent = extent;
	for (auto * target: {&target_a, &target_b})
	{
		vk::ImageCreateInfo image_info{
		        .imageType = vk::ImageType::e2D,
		        .format = format,
		        .extent = {extent.width, extent.height, 1},
		        .mipLevels = 1,
		        .arrayLayers = 2,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = vk::ImageTiling::eOptimal,
		        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		        .sharingMode = vk::SharingMode::eExclusive,
		        .initialLayout = vk::ImageLayout::eUndefined,
		};
		target->image = vk::raii::Image(device, image_info);
		auto requirements = target->image.getMemoryRequirements();
		vk::MemoryAllocateInfo alloc_info{
		        .allocationSize = requirements.size,
		        .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
		};
		target->memory = vk::raii::DeviceMemory(device, alloc_info);
		target->image.bindMemory(*target->memory, 0);
		target->views.clear();
		target->raster_fbs.clear();
		target->blur_fbs.clear();
		for (int layer = 0; layer < 2; ++layer)
		{
			vk::ImageViewCreateInfo view_info{
			        .image = *target->image,
			        .viewType = vk::ImageViewType::e2D,
			        .format = format,
			        .subresourceRange = {
			                .aspectMask = vk::ImageAspectFlagBits::eColor,
			                .baseMipLevel = 0,
			                .levelCount = 1,
			                .baseArrayLayer = (uint32_t)layer,
			                .layerCount = 1,
			        },
			};
			target->views.emplace_back(device, view_info);
		}
		// Named copies: framebuffer create-info stores the pointer, so the
		// handles must outlive the constructor calls (no &* temporaries).
		// Raster and blur passes share these framebuffers: identical
		// attachment spec (format/count/samples), which is all that
		// render-pass compatibility compares.
		vk::ImageView raw_views[2] = {*target->views[0], *target->views[1]};
		for (int layer = 0; layer < 2; ++layer)
		{
			vk::FramebufferCreateInfo fb_info{
			        .renderPass = *renderpass,
			        .attachmentCount = 1,
			        .pAttachments = &raw_views[layer],
			        .width = extent.width,
			        .height = extent.height,
			        .layers = 1,
			};
			target->raster_fbs.emplace_back(device, fb_info);
			fb_info.renderPass = *blur_renderpass;
			target->blur_fbs.emplace_back(device, fb_info);
		}
	}
	// Downsample/blur levels at half, quarter and eighth mask resolution
	// (exact halving: the mask extent is 64-quantized upstream). One set of
	// framebuffers each (blur render pass throughout); A/B above keep both.
	for (int li = 0; li < 3; ++li)
	{
		vk::Extent2D le = level_extent(extent, li + 1);
		for (auto * target: {&down_targets[li], &eblur_targets[li]})
		{
			vk::ImageCreateInfo image_info{
			        .imageType = vk::ImageType::e2D,
			        .format = format,
			        .extent = {le.width, le.height, 1},
			        .mipLevels = 1,
			        .arrayLayers = 2,
			        .samples = vk::SampleCountFlagBits::e1,
			        .tiling = vk::ImageTiling::eOptimal,
			        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
			        .sharingMode = vk::SharingMode::eExclusive,
			        .initialLayout = vk::ImageLayout::eUndefined,
			};
			target->image = vk::raii::Image(device, image_info);
			auto requirements = target->image.getMemoryRequirements();
			vk::MemoryAllocateInfo alloc_info{
			        .allocationSize = requirements.size,
			        .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
			};
			target->memory = vk::raii::DeviceMemory(device, alloc_info);
			target->image.bindMemory(*target->memory, 0);
			target->views.clear();
			target->blur_fbs.clear();
			for (int layer = 0; layer < 2; ++layer)
			{
				vk::ImageViewCreateInfo view_info{
				        .image = *target->image,
				        .viewType = vk::ImageViewType::e2D,
				        .format = format,
				        .subresourceRange = {
				                .aspectMask = vk::ImageAspectFlagBits::eColor,
				                .baseMipLevel = 0,
				                .levelCount = 1,
				                .baseArrayLayer = (uint32_t)layer,
				                .layerCount = 1,
				        },
				};
				target->views.emplace_back(device, view_info);
			}
			vk::ImageView raw_views[2] = {*target->views[0], *target->views[1]};
			for (int layer = 0; layer < 2; ++layer)
			{
			vk::FramebufferCreateInfo fb_info{
			        .renderPass = *blur_renderpass,
			        .attachmentCount = 1,
			        .pAttachments = &raw_views[layer],
			        .width = le.width,
			        .height = le.height,
			        .layers = 1,
			};
			target->blur_fbs.emplace_back(device, fb_info);
			}
		}
	}
	spdlog::info("Fiducial mask blur targets: {}x{}", extent.width, extent.height);
}

void feather_mask_renderer::flush_upload(vk::raii::CommandBuffer & cmd, mesh_buffers & mesh)
{
	if (not mesh.pending_upload)
		return;
	auto [vertex_bytes, index_bytes] = *mesh.pending_upload;
	mesh.pending_upload.reset();

	vk::BufferCopy vertex_copy{
	        .srcOffset = 0,
	        .dstOffset = 0,
	        .size = vertex_bytes,
	};
	cmd.copyBuffer(*mesh.staging_buffer, *mesh.vertex_buffer, vertex_copy);
	vk::BufferCopy index_copy{
	        .srcOffset = vertex_bytes,
	        .dstOffset = 0,
	        .size = index_bytes,
	};
	cmd.copyBuffer(*mesh.staging_buffer, *mesh.index_buffer, index_copy);

	std::array<vk::BufferMemoryBarrier, 2> barriers = {
	        vk::BufferMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead,
	                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                .buffer = *mesh.vertex_buffer,
	                .offset = 0,
	                .size = vertex_bytes,
	        },
	        vk::BufferMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                .dstAccessMask = vk::AccessFlagBits::eIndexRead,
	                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	                .buffer = *mesh.index_buffer,
	                .offset = 0,
	                .size = index_bytes,
	        },
	};
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                    vk::PipelineStageFlagBits::eVertexInput,
	                    {},
	                    {},
	                    barriers,
	                    {});
}

void feather_mask_renderer::update_source(vk::ImageView view, uint32_t set)
{
	vk::DescriptorImageInfo image_info{
	        .sampler = *sampler,
	        .imageView = view,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::WriteDescriptorSet write{
	        .dstSet = *descriptor_sets[set],
	        .dstBinding = 0,
	        .descriptorCount = 1,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .pImageInfo = &image_info,
	};
	std::array<vk::CopyDescriptorSet, 0> no_copies{};
	device.updateDescriptorSets(write, no_copies);
}

void feather_mask_renderer::record(vk::raii::CommandBuffer & cmd,
                                   vk::Image image,
                                   vk::Extent2D extent,
                                   const std::vector<instance_draw> & draws,
                                   bool rasterize,
                                   float feather_px,
                                   const std::vector<std::array<glm::vec3, 6>> & cutouts,
                                   const std::array<glm::mat4, 2> & cutout_mvp)
{
	auto set_full_viewport = [&](vk::Extent2D e) {
		cmd.setViewport(0, vk::Viewport{
		                        .x = 0,
		                        .y = 0,
		                        .width = (float)e.width,
		                        .height = (float)e.height,
		                        .minDepth = 0,
		                        .maxDepth = 1,
		                });
		cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = e});
	};

	// Same-queue read-after-write between passes (all images GENERAL).
	auto make_readable = [&](vk::Image img) {
		vk::ImageMemoryBarrier barrier{
		        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		        .oldLayout = vk::ImageLayout::eGeneral,
		        .newLayout = vk::ImageLayout::eGeneral,
		        .image = img,
		        .subresourceRange = {
		                .aspectMask = vk::ImageAspectFlagBits::eColor,
		                .levelCount = 1,
		                .layerCount = 2,
		        },
		};
		cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    vk::PipelineStageFlagBits::eFragmentShader,
		                    {},
		                    {},
		                    {},
		                    barrier);
	};

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
			                .attachmentCount = 1,
			                .pAttachments = &raw_view_0,
			                .width = extent.width,
			                .height = extent.height,
			                .layers = 1,
			        };
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		        [&] {
			        vk::FramebufferCreateInfo fb_info{
			                .renderPass = *renderpass,
			                .attachmentCount = 1,
			                .pAttachments = &raw_view_1,
			                .width = extent.width,
			                .height = extent.height,
			                .layers = 1,
			        };
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		}};
		it = targets.emplace(image, frame_targets(extent, std::move(views), std::move(framebuffers))).first;
	}

	vk::ClearValue clear{};
	clear.color.float32.fill(0);

	if (not rasterize)
	{
		// Bypass: clear-only passes, transparent mask, full stack hot.
		for (int eye = 0; eye < 2; ++eye)
		{
			vk::RenderPassBeginInfo begin_info{
			        .renderPass = *renderpass,
			        .framebuffer = *it->second.framebuffers[eye],
			        .renderArea = {.offset = {0, 0}, .extent = extent},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
			cmd.endRenderPass();
		}
	}
	else
	{
		if (meshes.empty())
			return;

		for (auto & [key, mesh]: meshes)
			flush_upload(cmd, mesh);
		ensure_targets(extent);

		// Tier from feather-px (see header): 0 hard edge, 1 direct
		// full-res, 2/4/8 downsampled. Band stays ±F/2 by construction,
		// so width is continuous across tiers; tap density stays in the
		// proven regime everywhere. Values past 128 clamp (documented).
		// Selected before Stage 1: tier 0 rasterizes straight into the
		// swapchain image and never touches the A intermediate.
		float f = feather_px;
		int tier = 1;
		float spread = 1.0f;
		if (f <= 0)
			tier = 0;
		else
		{
			if (f > 128)
				f = 128;
			if (f <= 16)
				spread = f / 8.f;
			else if (f <= 48)
			{
				tier = 2;
				spread = f / 24.f;
			}
			else if (f <= 96)
			{
				tier = 4;
				spread = f / 48.f;
			}
			else
			{
				tier = 8;
				spread = f / 96.f;
			}
		}

		// Stage 1: binary silhouettes into A (full resolution), one draw
		// per (mesh, instance, eye). Missing meshes (removed object racing
		// a queued draw) skip defensively. Tier 0 skips A entirely (its
		// raster below targets the swapchain image directly).
		if (tier != 0)
		{
			for (int eye = 0; eye < 2; ++eye)
			{
				vk::RenderPassBeginInfo begin_info{
				        .renderPass = *renderpass,
				        .framebuffer = *target_a.raster_fbs[eye],
				        .renderArea = {.offset = {0, 0}, .extent = extent},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
				set_full_viewport(extent);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
				for (const auto & d: draws)
				{
					auto mit = meshes.find(d.mesh);
					if (mit == meshes.end() or mit->second.index_count == 0)
						continue;
					cmd.bindVertexBuffers(0, (vk::Buffer)*mit->second.vertex_buffer, (vk::DeviceSize)0);
					cmd.bindIndexBuffer(*mit->second.index_buffer, 0, vk::IndexType::eUint32);
					cmd.pushConstants<raster_push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
					                               raster_push{.mvp = d.mvp[eye], .opacity = d.opacity});
					cmd.drawIndexed(mit->second.index_count, 1, 0, 0, 0);
				}
				cmd.endRenderPass();
			}
			make_readable(*target_a.image);
		}

		std::array<uint32_t, 0> no_offsets{};
		if (tier == 0)
		{
			// Hard edge: rasterize silhouettes straight into the swapchain
			// image (same pipeline, pushes and clear as Stage 1). This
			// replaces the old identity copy A -> swapchain, which burned
			// a fullscreen 5-fetch pass for zero effect (spread 0 hits
			// center with weights summing to 1). No blur passes.
			for (int eye = 0; eye < 2; ++eye)
			{
				vk::RenderPassBeginInfo begin_0{
				        .renderPass = *renderpass,
				        .framebuffer = *it->second.framebuffers[eye],
				        .renderArea = {.offset = {0, 0}, .extent = extent},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_0, vk::SubpassContents::eInline);
				set_full_viewport(extent);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
				for (const auto & d: draws)
				{
					auto mit = meshes.find(d.mesh);
					if (mit == meshes.end() or mit->second.index_count == 0)
						continue;
					cmd.bindVertexBuffers(0, (vk::Buffer)*mit->second.vertex_buffer, (vk::DeviceSize)0);
					cmd.bindIndexBuffer(*mit->second.index_buffer, 0, vk::IndexType::eUint32);
					cmd.pushConstants<raster_push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
					                               raster_push{.mvp = d.mvp[eye], .opacity = d.opacity});
					cmd.drawIndexed(mit->second.index_count, 1, 0, 0, 0);
				}
				cmd.endRenderPass();
			}
		}
		else if (tier == 1)
		{
		// Stages 2+3: separable Gaussian H into B, V into the swapchain.
		// One descriptor set per eye (see header): re-pointed per pass.
		// (Kept at this indent deliberately: byte-identical to the proven
		// path; the tier branches above/below own their nesting.)
		std::array<uint32_t, 0> no_offsets{};
		blur_push base{
		        .texel = {1.f / extent.width, 1.f / extent.height},
		        .spread = spread,
		};
		for (int eye = 0; eye < 2; ++eye)
		{
			update_source(*target_a.views[eye], eye);
			vk::RenderPassBeginInfo begin_h{
			        .renderPass = *blur_renderpass,
			        .framebuffer = *target_b.blur_fbs[eye],
			        .renderArea = {.offset = {0, 0}, .extent = extent},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			cmd.beginRenderPass(begin_h, vk::SubpassContents::eInline);
			set_full_viewport(extent);
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
			std::array<vk::DescriptorSet, 1> sets_h{*descriptor_sets[eye]};
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_h, no_offsets);
			blur_push push = base;
			push.dir[0] = 1;
			push.dir[1] = 0;
			cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
			cmd.draw(3, 1, 0, 0);
			cmd.endRenderPass();
		}
		make_readable(*target_b.image);

		for (int eye = 0; eye < 2; ++eye)
		{
			update_source(*target_b.views[eye], 2 + eye);
			vk::RenderPassBeginInfo begin_v{
			        .renderPass = *blur_renderpass,
			        .framebuffer = *it->second.framebuffers[eye],
			        .renderArea = {.offset = {0, 0}, .extent = extent},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			cmd.beginRenderPass(begin_v, vk::SubpassContents::eInline);
			set_full_viewport(extent);
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
			std::array<vk::DescriptorSet, 1> sets_v{*descriptor_sets[2 + eye]};
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_v, no_offsets);
			blur_push push = base;
			push.dir[0] = 0;
			push.dir[1] = 1;
			cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
			cmd.draw(3, 1, 0, 0);
			cmd.endRenderPass();
		}
		} // tier == 1
		else
		{
			// Tiered path: downsample chain, blur at 1/k, upscale submit.
			// Levels 1..N from the selected tier; sizes shared with ensure
			// via level_extent() (exact halving required for the box map).
			std::array<uint32_t, 0> no_offsets{};
			int levels = 0;
			for (int k = tier; k >= 2; k >>= 1)
				++levels;
			std::vector<vk::raii::ImageView> * down_src_views = &target_a.views;
			vk::Extent2D down_src_extent = extent;
			for (int l = 1; l <= levels; ++l)
			{
				blur_target & dst = down_targets[l - 1];
				vk::Extent2D de = level_extent(extent, l);
				for (int eye = 0; eye < 2; ++eye)
				{
					update_source(*(*down_src_views)[eye], 4 + (l - 1) * 2 + eye);
					vk::RenderPassBeginInfo begin_down{
					        .renderPass = *blur_renderpass,
					        .framebuffer = *dst.blur_fbs[eye],
					        .renderArea = {.offset = {0, 0}, .extent = de},
					        .clearValueCount = 1,
					        .pClearValues = &clear,
					};
					cmd.beginRenderPass(begin_down, vk::SubpassContents::eInline);
					set_full_viewport(de);
					cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *downsample_pipeline);
					std::array<vk::DescriptorSet, 1> sets_d{*descriptor_sets[4 + (l - 1) * 2 + eye]};
					cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_d, no_offsets);
					blur_push push{};
					push.texel[0] = 1.f / down_src_extent.width;
					push.texel[1] = 1.f / down_src_extent.height;
					cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
					cmd.draw(3, 1, 0, 0);
					cmd.endRenderPass();
				}
				make_readable(*dst.image);
				down_src_views = &dst.views;
				down_src_extent = de;
			}

			// H blur at tier res into E, V upscale into swapchain. The V
			// texel stays 1/tier-size while rendering fullscreen: the
			// sampler's bilinear upscale is the final step, free.
			blur_target & eblur = eblur_targets[levels - 1];
			vk::Extent2D te = level_extent(extent, levels);
			blur_push base{
			        .texel = {1.f / te.width, 1.f / te.height},
			        .spread = spread,
			};
			for (int eye = 0; eye < 2; ++eye)
			{
				update_source(*(*down_src_views)[eye], eye);
				vk::RenderPassBeginInfo begin_h{
				        .renderPass = *blur_renderpass,
				        .framebuffer = *eblur.blur_fbs[eye],
				        .renderArea = {.offset = {0, 0}, .extent = te},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_h, vk::SubpassContents::eInline);
				set_full_viewport(te);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
				std::array<vk::DescriptorSet, 1> sets_h{*descriptor_sets[eye]};
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_h, no_offsets);
				blur_push push = base;
				push.dir[0] = 1;
				push.dir[1] = 0;
				cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
				cmd.draw(3, 1, 0, 0);
				cmd.endRenderPass();
			}
			make_readable(*eblur.image);

			for (int eye = 0; eye < 2; ++eye)
			{
				update_source(*eblur.views[eye], 2 + eye);
				vk::RenderPassBeginInfo begin_v{
				        .renderPass = *blur_renderpass,
				        .framebuffer = *it->second.framebuffers[eye],
				        .renderArea = {.offset = {0, 0}, .extent = extent},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_v, vk::SubpassContents::eInline);
				set_full_viewport(extent);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
				std::array<vk::DescriptorSet, 1> sets_v{*descriptor_sets[2 + eye]};
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_v, no_offsets);
				blur_push push = base;
				push.dir[0] = 0;
				push.dir[1] = 1;
				cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
				cmd.draw(3, 1, 0, 0);
				cmd.endRenderPass();
			}
		} // tiered
	}

	// Marker window cutouts: crisp post-blur punch. Re-begins the finished
	// swapchain framebuffers with the LOAD pass and stamps each quad at
	// FULL alpha (mask 1 = reality window): holes show passthrough, not
	// game. No blur pass touches them, so edges stay pixel-exact. Debug
	// only; rasterize=false (bypass) skips them with the silhouettes.
	if (not cutouts.empty() and rasterize)
	{
		if (not cutout_verts)
		{
			cutout_verts = buffer_allocation{
			        device,
			        vk::BufferCreateInfo{
			                .size = sizeof(glm::vec3) * 6,
			                .usage = vk::BufferUsageFlagBits::eVertexBuffer,
			        },
			        VmaAllocationCreateInfo{
			                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
			                .usage = VMA_MEMORY_USAGE_AUTO,
			        },
			        "feather_mask cutout",
			};
		}
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
		cmd.bindVertexBuffers(0, (vk::Buffer)cutout_verts, (vk::DeviceSize)0);
		for (const auto & quad: cutouts)
		{
			std::memcpy(cutout_verts.map(), quad.data(), sizeof(glm::vec3) * 6);
			for (int eye = 0; eye < 2; ++eye)
			{
				// Tighten to the quad's pixel bounds (same NDC mapping the
				// viewport itself uses, so alignment is exact by
				// construction): fullscreen LOAD/STORE traffic for a small
				// debug rect would cost a full image round-trip per group.
				// Any corner behind the camera falls back to fullscreen.
				vk::Rect2D area{{0, 0}, extent};
				bool behind = false;
				float minx = (float)extent.width, miny = (float)extent.height;
				float maxx = 0, maxy = 0;
				for (const auto & v: quad)
				{
					glm::vec4 c = cutout_mvp[eye] * glm::vec4(v, 1);
					if (c.w <= 0)
					{
						behind = true;
						break;
					}
					float px = (c.x / c.w * 0.5f + 0.5f) * (float)extent.width;
					float py = (c.y / c.w * 0.5f + 0.5f) * (float)extent.height;
					minx = std::min(minx, px);
					miny = std::min(miny, py);
					maxx = std::max(maxx, px);
					maxy = std::max(maxy, py);
				}
				if (not behind)
				{
					int32_t x0 = std::clamp<int32_t>((int32_t)std::floor(minx) - 1, 0, (int32_t)extent.width);
					int32_t y0 = std::clamp<int32_t>((int32_t)std::floor(miny) - 1, 0, (int32_t)extent.height);
					int32_t x1 = std::clamp<int32_t>((int32_t)std::ceil(maxx) + 1, 0, (int32_t)extent.width);
					int32_t y1 = std::clamp<int32_t>((int32_t)std::ceil(maxy) + 1, 0, (int32_t)extent.height);
					if (x1 <= x0 or y1 <= y0)
						continue;
					area = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
				}
				vk::RenderPassBeginInfo begin_cut{
				        .renderPass = *cutout_renderpass,
				        .framebuffer = *it->second.framebuffers[eye],
				        .renderArea = area,
				        .clearValueCount = 0,
				};
				cmd.beginRenderPass(begin_cut, vk::SubpassContents::eInline);
				set_full_viewport(extent);
				cmd.setScissor(0, area);
				cmd.pushConstants<raster_push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
				                               raster_push{.mvp = cutout_mvp[eye], .opacity = 1});
				cmd.draw(6, 1, 0, 0);
				cmd.endRenderPass();
			}
		}
	}

	vk::ImageMemoryBarrier barrier{
	        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	        .dstAccessMask = vk::AccessFlagBits::eMemoryRead,
	        .oldLayout = vk::ImageLayout::eGeneral,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = image,
	        .subresourceRange = {
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 2,
	        },
	};
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
	                    vk::PipelineStageFlagBits::eBottomOfPipe,
	                    {},
	                    {},
	                    {},
	                    barrier);
}
