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
#include <chrono>
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

// RAII section timer for the record() diagnostics (mask_stage_cpu): adds
// the scoped wall time in milliseconds to *acc on destruction.
struct section_clock
{
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	double * acc;
	explicit section_clock(double * a) :
	        acc(a) {}
	~section_clock()
	{
		*acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
};

// Single-channel intermediates (mask alpha only): quarter the bytes of
// RGBA8 at identical 8-bit precision. Submitted images stay RGBA8 (the
// blend-factor vocabulary needs an alpha channel).
constexpr vk::Format blur_format = vk::Format::eR8Unorm;

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

	// R8 variant of the raster pass for the single-channel intermediates
	// (same CLEAR/STORE shape, R8 attachment).
	vk::AttachmentDescription raster_r8_attachment{
	        .format = blur_format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eClear,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference raster_r8_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription raster_r8_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &raster_r8_ref,
	};
	vk::RenderPassCreateInfo raster_r8_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &raster_r8_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &raster_r8_subpass,
	};
	raster_r8_renderpass = vk::raii::RenderPass(device, raster_r8_rp_info);

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

	// R8 raster pipeline: same shaders (mask.frag replicates opacity to
	// all channels) and layout, R8 render pass.
	vk::GraphicsPipelineCreateInfo raster_r8_pipeline_info{
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
	        .renderPass = *raster_r8_renderpass,
	};
	raster_r8_pipeline = vk::raii::Pipeline(device, nullptr, raster_r8_pipeline_info);

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

	// R8 variant of the blur pass for the intermediates (same DONT_CARE
	// shape). The RGBA8 pass above serves only the V upscale into
	// swapchain images now.
	vk::AttachmentDescription blur_r8_attachment{
	        .format = blur_format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eDontCare,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference blur_r8_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription blur_r8_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &blur_r8_ref,
	};
	vk::RenderPassCreateInfo blur_r8_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &blur_r8_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &blur_r8_subpass,
	};
	blur_r8_renderpass = vk::raii::RenderPass(device, blur_r8_rp_info);

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

	// R8 blur pipeline: same shaders/layout (format-free), R8 pass. Serves
	// every blur except the V upscale (which targets RGBA8 swapchains).
	vk::GraphicsPipelineCreateInfo blur_r8_pipeline_info{
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
	        .renderPass = *blur_r8_renderpass,
	};
	blur_r8_pipeline = vk::raii::Pipeline(device, nullptr, blur_r8_pipeline_info);

	// Box-downsample pipeline: same fullscreen vertex shader, same layout
	// (reads only the push prefix); R8 throughout (intermediates only).
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
	        .renderPass = *blur_r8_renderpass,
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
	// Model-space bounds for the stream-side frustum skip (upload-time
	// only; per-frame tests transform the 8 corners by the draw MVP).
	glm::vec3 lo{soup.vertices[0].x, soup.vertices[0].y, soup.vertices[0].z};
	glm::vec3 hi = lo;
	for (const auto & v: soup.vertices)
	{
		lo = glm::min(lo, glm::vec3(v.x, v.y, v.z));
		hi = glm::max(hi, glm::vec3(v.x, v.y, v.z));
	}
	mesh.aabb_min = lo;
	mesh.aabb_max = hi;
}

std::optional<std::pair<glm::vec3, glm::vec3>> feather_mask_renderer::mesh_bounds(const std::string & key) const
{
	auto it = meshes.find(key);
	if (it == meshes.end())
		return std::nullopt;
	return std::make_pair(it->second.aabb_min, it->second.aabb_max);
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
	// A/B intermediates at half mask resolution (the mask extent is
	// 64-quantized upstream, hence evenly divisible); tier-1 raster and
	// H run small, V writes the half-res submitted image (the compositor
	// upscales to full).
	vk::Extent2D half_targets = level_extent(extent, 1);
	for (auto * target: {&target_a, &target_b})
	{
		// Views/framebuffers must die before the image they reference
		// (the image assignment below destroys the old image).
		target->views.clear();
		target->raster_fbs.clear();
		target->blur_fbs.clear();
		vk::ImageCreateInfo image_info{
		        .imageType = vk::ImageType::e2D,
		        .format = blur_format,
		        .extent = {half_targets.width, half_targets.height, 1},
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
		for (int layer = 0; layer < 2; ++layer)
		{
			vk::ImageViewCreateInfo view_info{
			        .image = *target->image,
			        .viewType = vk::ImageViewType::e2D,
			        .format = blur_format,
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
			        .renderPass = *raster_r8_renderpass,
			        .attachmentCount = 1,
			        .pAttachments = &raw_views[layer],
			        .width = half_targets.width,
			        .height = half_targets.height,
			        .layers = 1,
			};
			target->raster_fbs.emplace_back(device, fb_info);
			fb_info.renderPass = *blur_r8_renderpass;
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
		        .format = blur_format,
		        .extent = {le.width, le.height, 1},
			        .mipLevels = 1,
			        .arrayLayers = 2,
			        .samples = vk::SampleCountFlagBits::e1,
			        .tiling = vk::ImageTiling::eOptimal,
			        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
			        .sharingMode = vk::SharingMode::eExclusive,
			        .initialLayout = vk::ImageLayout::eUndefined,
			};
			target->views.clear();
			target->blur_fbs.clear();
			target->image = vk::raii::Image(device, image_info);
			auto requirements = target->image.getMemoryRequirements();
			vk::MemoryAllocateInfo alloc_info{
			        .allocationSize = requirements.size,
			        .memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
			};
			target->memory = vk::raii::DeviceMemory(device, alloc_info);
			target->image.bindMemory(*target->memory, 0);
			for (int layer = 0; layer < 2; ++layer)
			{
				vk::ImageViewCreateInfo view_info{
				        .image = *target->image,
				        .viewType = vk::ImageViewType::e2D,
				        .format = blur_format,
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
			        .renderPass = *blur_r8_renderpass,
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
	spdlog::info("Fiducial mask blur targets: {}x{} (A/B at half, levels below)", extent.width, extent.height);
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
                                   const std::array<glm::mat4, 2> & cutout_mvp,
                                   mask_stage_cpu * cpu_stats,
                                   vk::Extent2D out_extent)
{
	auto t_total0 = std::chrono::steady_clock::now();
	double ms_raster = 0, ms_blur = 0, ms_punch = 0;
	size_t n_draws = 0;
	int tier_out = -1;
	auto fill_stats = [&] {
		if (cpu_stats)
		{
			cpu_stats->raster_ms = ms_raster;
			cpu_stats->blur_ms = ms_blur;
			cpu_stats->punch_ms = ms_punch;
			cpu_stats->total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_total0).count();
			cpu_stats->tier = tier_out;
			cpu_stats->draws = n_draws;
		}
	};

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
	// Swapchain-image targets track the submitted (output) size, which is
	// half for feathered groups; intermediates below track full extent.
	if (it == targets.end() or it->second.extent.width != out_extent.width or it->second.extent.height != out_extent.height)
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
			                .width = out_extent.width,
			                .height = out_extent.height,
			                .layers = 1,
			        };
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		        [&] {
			        vk::FramebufferCreateInfo fb_info{
			                .renderPass = *renderpass,
			                .attachmentCount = 1,
			                .pAttachments = &raw_view_1,
			                .width = out_extent.width,
			                .height = out_extent.height,
			                .layers = 1,
			        };
			        return vk::raii::Framebuffer(device, fb_info);
		        }(),
		}};
		it = targets.emplace(image, frame_targets(out_extent, std::move(views), std::move(framebuffers))).first;
	}

	vk::ClearValue clear{};
	clear.color.float32.fill(0);

	// Silhouette raster shared by every path: one draw per (mesh,
	// instance) into one eye's framebuffer at the given extent. Missing
	// meshes (removed object racing a queued draw) skip defensively. NDC
	// is resolution-independent, so the same draws serve full- and
	// tier-res targets with only the viewport changing. Per-eye callers
	// complete each eye's full chain before the next eye starts (eye-outer
	// order), keeping that eye's tiles hot.
	auto raster_silhouette = [&](vk::Framebuffer fb, vk::Extent2D e, int eye, vk::raii::Pipeline & pipe, vk::raii::RenderPass & rp) {
		vk::RenderPassBeginInfo begin_info{
		        .renderPass = *rp,
		        .framebuffer = fb,
		        .renderArea = {.offset = {0, 0}, .extent = e},
		        .clearValueCount = 1,
		        .pClearValues = &clear,
		};
		cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
		set_full_viewport(e);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
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
			++n_draws;
		}
		cmd.endRenderPass();
	};

	if (not rasterize)
	{
		section_clock raster_clk(&ms_raster);
		// Bypass: clear-only passes, transparent mask, full stack hot.
		for (int eye = 0; eye < 2; ++eye)
		{
			vk::RenderPassBeginInfo begin_info{
			        .renderPass = *renderpass,
			        .framebuffer = *it->second.framebuffers[eye],
			        .renderArea = {.offset = {0, 0}, .extent = out_extent},
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
		{
			fill_stats();
			return;
		}

		for (auto & [key, mesh]: meshes)
			flush_upload(cmd, mesh);
		ensure_targets(extent);

		// Tier from feather-px (see header): 0 hard edge, 1 half-res
		// blur, 2/4/8 downsampled. Every feathered group submits a
		// half-res image (V writes half pixels, the compositor
		// bilinear-upscales to full on submit), so the blur works in
		// 1/k-size texels (k = 2 tier-1, 4 tier-2/4, 8 tier-8) and the
		// screen-space band is 2k*sqrt(2)*spread px. Divisor 12*k keeps
		// the band continuous across tiers (within float rounding) with
		// tap density flat everywhere. Values past 128 clamp (documented).
		// Selected before raster: tier 0 goes straight into the swapchain
		// image, tier 1 via the half-res A/B intermediates, tiered via a
		// level-2 (tier 2) or level-1 (tier 4/8) seed.
		float f = feather_px > 128.f ? 128.f : feather_px;
		int tier = tier_for_feather(feather_px);
		float spread = 1.0f;
		if (tier == 1)
		{
			// k = 2: band 4*sqrt(2)*spread px.
			spread = f / 24.f;
		}
		else if (tier == 2)
		{
			// k = 4: band 8*sqrt(2)*spread px, same slope as tier 1.
			spread = f / 48.f;
		}
		else if (tier == 4)
			spread = f / 48.f;
		else if (tier == 8)
		{
			// k = 8: band 16*sqrt(2)*spread px, same slope again.
			spread = f / 96.f;
		}

		// Half-res working extent for the A/B intermediates (the mask
		// extent is 64-quantized upstream, hence evenly divisible).
		vk::Extent2D half_extent = level_extent(extent, 1);

		// Raster happens inside each path's own eye loop below (tier 1
		// into half-res A, tiered as a level seed, tier 0 direct into the
		// swapchain image).
		tier_out = tier;

		std::array<uint32_t, 0> no_offsets{};
		if (tier == 0)
		{
			section_clock raster_clk(&ms_raster);
			// Hard edge: rasterize silhouettes straight into the swapchain
			// image. Exact, one raster pass per eye, no blur passes.
			// (Tier-0 groups submit full, so out matches working here.)
			for (int eye = 0; eye < 2; ++eye)
				raster_silhouette(*it->second.framebuffers[eye], out_extent, eye, pipeline, renderpass);
		}
		else if (tier == 1)
		{
		// Stages 1+2+3 per eye: raster into half-res A, H into half-res B,
		// V upscale into the swapchain image. Eye-outer order (each eye's
		// chain completes before the next eye starts) keeps that eye's
		// tiles hot; same passes, pushes and clears as the old per-pass
		// loops. One descriptor set per eye (see header): re-pointed per
		// pass, each written once per frame.
		std::array<uint32_t, 0> no_offsets{};
		blur_push base{
		        .texel = {1.f / half_extent.width, 1.f / half_extent.height},
		        .spread = spread,
		};
		for (int eye = 0; eye < 2; ++eye)
		{
			{
				section_clock raster_clk(&ms_raster);
				raster_silhouette(*target_a.raster_fbs[eye], half_extent, eye, raster_r8_pipeline, raster_r8_renderpass);
			}
			make_readable(*target_a.image);
			section_clock blur_clk(&ms_blur);
			update_source(*target_a.views[eye], eye);
			vk::RenderPassBeginInfo begin_h{
			        .renderPass = *blur_r8_renderpass,
			        .framebuffer = *target_b.blur_fbs[eye],
			        .renderArea = {.offset = {0, 0}, .extent = half_extent},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			cmd.beginRenderPass(begin_h, vk::SubpassContents::eInline);
			set_full_viewport(half_extent);
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_r8_pipeline);
			std::array<vk::DescriptorSet, 1> sets_h{*descriptor_sets[eye]};
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_h, no_offsets);
			blur_push push = base;
			push.dir[0] = 1;
			push.dir[1] = 0;
			cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
			cmd.draw(3, 1, 0, 0);
			cmd.endRenderPass();
			make_readable(*target_b.image);
			update_source(*target_b.views[eye], 2 + eye);
			vk::RenderPassBeginInfo begin_v{
			        .renderPass = *blur_renderpass,
			        .framebuffer = *it->second.framebuffers[eye],
			        .renderArea = {.offset = {0, 0}, .extent = out_extent},
			        .clearValueCount = 1,
			        .pClearValues = &clear,
			};
			cmd.beginRenderPass(begin_v, vk::SubpassContents::eInline);
			set_full_viewport(out_extent);
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
			std::array<vk::DescriptorSet, 1> sets_v{*descriptor_sets[2 + eye]};
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_v, no_offsets);
			blur_push push_v = base;
			push_v.dir[0] = 0;
			push_v.dir[1] = 1;
			cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push_v);
			cmd.draw(3, 1, 0, 0);
			cmd.endRenderPass();
		}
		} // tier == 1
		else
		{
			// Tiered path per eye: seed-rasterize below full res, downsample
			// chain to the blur level, blur at 1/k, upscale submit. Eye-outer
			// order keeps each eye's tiles hot (same rationale as tier 1).
			// Tier 2 seeds at quarter (level 2); higher tiers seed at half,
			// so the H working level is the deeper of the seed level and
			// the tier's natural level. down_targets[N].blur_fbs serves the
			// seed raster: identical attachment spec, which is all that
			// render-pass compatibility compares (house idiom, cf. A/B).
			int seed_level = (tier == 2) ? 2 : 1;
			vk::Extent2D de_seed = level_extent(extent, seed_level);
			std::array<uint32_t, 0> no_offsets{};
			int levels = 0;
			for (int k = tier; k >= 2; k >>= 1)
				++levels;
			int blur_level = std::max(levels, seed_level);
			blur_target & eblur = eblur_targets[blur_level - 1];
			vk::Extent2D te = level_extent(extent, blur_level);
			blur_push base{
			        .texel = {1.f / te.width, 1.f / te.height},
			        .spread = spread,
			};
			for (int eye = 0; eye < 2; ++eye)
			{
				{
					section_clock raster_clk(&ms_raster);
					raster_silhouette(*down_targets[seed_level - 1].blur_fbs[eye], de_seed, eye, raster_r8_pipeline, raster_r8_renderpass);
				}
				make_readable(*down_targets[seed_level - 1].image);
				section_clock blur_clk(&ms_blur);
				// Levels seed_level+1..N from the selected tier; sizes shared
				// with ensure via level_extent() (exact halving required for
				// the box map).
				vk::ImageView down_src = *down_targets[seed_level - 1].views[eye];
				vk::Extent2D down_src_extent = de_seed;
				for (int l = seed_level + 1; l <= levels; ++l)
				{
					blur_target & dst = down_targets[l - 1];
					vk::Extent2D de = level_extent(extent, l);
					update_source(down_src, 4 + (l - 1) * 2 + eye);
					vk::RenderPassBeginInfo begin_down{
					        .renderPass = *blur_r8_renderpass,
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
					make_readable(*dst.image);
					down_src = *dst.views[eye];
					down_src_extent = de;
				}
				update_source(down_src, eye);
				vk::RenderPassBeginInfo begin_h{
				        .renderPass = *blur_r8_renderpass,
				        .framebuffer = *eblur.blur_fbs[eye],
				        .renderArea = {.offset = {0, 0}, .extent = te},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_h, vk::SubpassContents::eInline);
				set_full_viewport(te);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_r8_pipeline);
				std::array<vk::DescriptorSet, 1> sets_h{*descriptor_sets[eye]};
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_h, no_offsets);
				blur_push push = base;
				push.dir[0] = 1;
				push.dir[1] = 0;
				cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
				cmd.draw(3, 1, 0, 0);
				cmd.endRenderPass();
				make_readable(*eblur.image);
				update_source(*eblur.views[eye], 2 + eye);
				vk::RenderPassBeginInfo begin_v{
				        .renderPass = *blur_renderpass,
				        .framebuffer = *it->second.framebuffers[eye],
				        .renderArea = {.offset = {0, 0}, .extent = out_extent},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_v, vk::SubpassContents::eInline);
				set_full_viewport(out_extent);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *blur_pipeline);
				std::array<vk::DescriptorSet, 1> sets_v{*descriptor_sets[2 + eye]};
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *blur_layout, 0, sets_v, no_offsets);
				blur_push push_v = base;
				push_v.dir[0] = 0;
				push_v.dir[1] = 1;
				cmd.pushConstants<blur_push>(*blur_layout, vk::ShaderStageFlagBits::eFragment, 0, push_v);
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
		// Execution barrier: the V upscale (or tier-0 raster) above wrote
		// the swapchain image as COLOR_ATTACHMENT; the LOAD cutout pass
		// below reads it. Without this a tile GPU may LOAD stale tiles.
		vk::ImageMemoryBarrier cut_barrier{
		        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
		        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite,
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
		                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
		                    {},
		                    {},
		                    {},
		                    cut_barrier);
		// One vertex buffer for every quad, uploaded once: record() only
		// records, so per-quad memcpys before submit would leave every
		// draw reading the last quad. Grown on demand (debug path only).
		vk::DeviceSize cut_need = sizeof(glm::vec3) * 6 * cutouts.size();
		if (not cutout_verts or cutout_verts.size() < cut_need)
		{
			cutout_verts = buffer_allocation{
			        device,
			        vk::BufferCreateInfo{
			                .size = cut_need,
			                .usage = vk::BufferUsageFlagBits::eVertexBuffer,
			        },
			        VmaAllocationCreateInfo{
			                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
			                .usage = VMA_MEMORY_USAGE_AUTO,
			        },
			        "feather_mask cutout",
			};
		}
		for (size_t qi = 0; qi < cutouts.size(); ++qi)
			std::memcpy((char *)cutout_verts.map() + qi * sizeof(glm::vec3) * 6, cutouts[qi].data(), sizeof(glm::vec3) * 6);
		// House idiom (cf. gpu_buffer): unmap flushes host writes for GPU read.
		cutout_verts.unmap();
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
		cmd.bindVertexBuffers(0, (vk::Buffer)cutout_verts, (vk::DeviceSize)0);
		section_clock punch_clk(&ms_punch);
		for (size_t qi = 0; qi < cutouts.size(); ++qi)
		{
			const auto & quad = cutouts[qi];
			for (int eye = 0; eye < 2; ++eye)
			{
				// Tighten to the quad's pixel bounds (same NDC mapping the
				// viewport itself uses, so alignment is exact by
				// construction): fullscreen LOAD/STORE traffic for a small
				// debug rect would cost a full image round-trip per group.
				// Any corner behind the camera falls back to fullscreen.
				// Bounds track the submitted (output) size, like the
				// viewport below: NDC maps consistently at any size.
				vk::Rect2D area{{0, 0}, out_extent};
				bool behind = false;
				float minx = (float)out_extent.width, miny = (float)out_extent.height;
				float maxx = 0, maxy = 0;
				for (const auto & v: quad)
				{
					glm::vec4 c = cutout_mvp[eye] * glm::vec4(v, 1);
					if (c.w <= 0)
					{
						behind = true;
						break;
					}
					float px = (c.x / c.w * 0.5f + 0.5f) * (float)out_extent.width;
					float py = (c.y / c.w * 0.5f + 0.5f) * (float)out_extent.height;
					minx = std::min(minx, px);
					miny = std::min(miny, py);
					maxx = std::max(maxx, px);
					maxy = std::max(maxy, py);
				}
				if (not behind)
				{
					int32_t x0 = std::clamp<int32_t>((int32_t)std::floor(minx) - 1, 0, (int32_t)out_extent.width);
					int32_t y0 = std::clamp<int32_t>((int32_t)std::floor(miny) - 1, 0, (int32_t)out_extent.height);
					int32_t x1 = std::clamp<int32_t>((int32_t)std::ceil(maxx) + 1, 0, (int32_t)out_extent.width);
					int32_t y1 = std::clamp<int32_t>((int32_t)std::ceil(maxy) + 1, 0, (int32_t)out_extent.height);
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
				set_full_viewport(out_extent);
				cmd.setScissor(0, area);
				cmd.pushConstants<raster_push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
				                               raster_push{.mvp = cutout_mvp[eye], .opacity = 1});
				cmd.draw(6, 1, (uint32_t)(qi * 6), 0);
				cmd.endRenderPass();
			}
		}
	}

	// All mask passes recorded (barrier below is submit bookkeeping, not
	// mask work): stamp the diagnostics before leaving.
	fill_stats();

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
