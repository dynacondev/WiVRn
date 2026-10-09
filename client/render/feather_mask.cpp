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
#include <array>
#include <chrono>
#include <cmath>
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

// Silhouette raster push block. Mirrors mask.vert PushConstants (mvp)
// plus opacity: mask.frag (tier 0) writes it as mask alpha, seed.frag
// ignores it (seeds are binary), and the SDF composite takes the group
// minimum as its uniform opacity.
struct raster_push
{
	glm::mat4 mvp;
	float opacity = 1;
};

// Jump Flood push block. Mirrors jfa.frag.glsl Push (16 bytes).
struct jfa_push
{
	float texel[2];
	float step;
	float pad = 0;
};

// Analytic composite push block. Mirrors sdf_composite.frag.glsl Push
// (24 bytes).
struct sdf_composite_push
{
	float sdf_dim[2];
	float feather_px;
	float opacity;
	float px_per_texel;
	float pad = 0;
};

// SDF seed/field storage: float32 RG (coordinates exact, INF-safe).
constexpr vk::Format sdf_format = vk::Format::eR32G32Sfloat;

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

	// SDF seed pass: RG32F, CLEARs to INF ("no seed"), stores GENERAL
	// (the composite samples it back for the inside/outside sign).
	vk::AttachmentDescription seed_attachment{
	        .format = sdf_format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eClear,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference seed_color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription seed_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &seed_color_ref,
	};
	vk::RenderPassCreateInfo seed_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &seed_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &seed_subpass,
	};
	seed_renderpass = vk::raii::RenderPass(device, seed_rp_info);

	// SDF flood pass: RG32F, DONT_CARE load (every texel is rewritten by
	// the fullscreen triangle), stores GENERAL for the next iteration.
	vk::AttachmentDescription sdf_attachment{
	        .format = sdf_format,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eDontCare,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eGeneral,
	};
	vk::AttachmentReference sdf_color_ref{
	        .attachment = 0,
	        .layout = vk::ImageLayout::eColorAttachmentOptimal,
	};
	vk::SubpassDescription sdf_subpass{
	        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &sdf_color_ref,
	};
	vk::RenderPassCreateInfo sdf_rp_info{
	        .attachmentCount = 1,
	        .pAttachments = &sdf_attachment,
	        .subpassCount = 1,
	        .pSubpasses = &sdf_subpass,
	};
	sdf_renderpass = vk::raii::RenderPass(device, sdf_rp_info);

	// Seed pipeline: same mesh vertex input as the raster pass (mask.vert
	// transforms by pc.mvp), seed.frag emits fragcoords. Shares the raster
	// pipeline layout (push block identical; the fragment stage just
	// ignores opacity).
	auto seed_frag = load_shader(device, "seed.frag");
	vk::PipelineShaderStageCreateInfo seed_stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **seed_frag,
	                .pName = "main",
	        },
	};
	vk::GraphicsPipelineCreateInfo seed_pipeline_info{
	        .stageCount = 2,
	        .pStages = seed_stages,
	        .pVertexInputState = &vertex_input,
	        .pInputAssemblyState = &input_assembly,
	        .pViewportState = &viewport_state,
	        .pRasterizationState = &rasterization,
	        .pMultisampleState = &multisample,
	        .pColorBlendState = &blend,
	        .pDynamicState = &dynamic,
	        .layout = *pipeline_layout,
	        .renderPass = *seed_renderpass,
	};
	seed_pipeline = vk::raii::Pipeline(device, nullptr, seed_pipeline_info);

	auto fullscreen_vert = load_shader(device, "blur.vert");
	auto jfa_frag = load_shader(device, "jfa.frag");
	auto composite_frag = load_shader(device, "sdf_composite.frag");
	vk::PipelineShaderStageCreateInfo jfa_stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **fullscreen_vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **jfa_frag,
	                .pName = "main",
	        },
	};
	vk::PipelineShaderStageCreateInfo composite_stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = **fullscreen_vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = **composite_frag,
	                .pName = "main",
	        },
	};

	vk::PipelineVertexInputStateCreateInfo fs_vertex_input{};
	vk::PipelineInputAssemblyStateCreateInfo fs_input_assembly{
	        .topology = vk::PrimitiveTopology::eTriangleList,
	};
	vk::PipelineViewportStateCreateInfo fs_viewport_state{
	        .viewportCount = 1,
	        .scissorCount = 1,
	};
	vk::PipelineRasterizationStateCreateInfo fs_rasterization{
	        .polygonMode = vk::PolygonMode::eFill,
	        .cullMode = vk::CullModeFlagBits::eNone,
	        .frontFace = vk::FrontFace::eCounterClockwise,
	        .lineWidth = 1,
	};
	vk::PipelineMultisampleStateCreateInfo fs_multisample{
	        .rasterizationSamples = vk::SampleCountFlagBits::e1,
	};
	vk::PipelineColorBlendAttachmentState fs_blend_attachment{
	        .blendEnable = VK_FALSE,
	        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
	};
	vk::PipelineColorBlendStateCreateInfo fs_blend{
	        .attachmentCount = 1,
	        .pAttachments = &fs_blend_attachment,
	};
	vk::DynamicState fs_dynamics[2] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo fs_dynamic{
	        .dynamicStateCount = 2,
	        .pDynamicStates = fs_dynamics,
	};

	// JFA set: single flood-source sampler. Composite set: flood field
	// (linear, binding 0) + untouched seed (nearest, binding 1).
	vk::DescriptorSetLayoutBinding jfa_binding{
	        .binding = 0,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 1,
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	};
	vk::DescriptorSetLayoutCreateInfo jfa_set_layout_info{
	        .bindingCount = 1,
	        .pBindings = &jfa_binding,
	};
	jfa_set_layout = vk::raii::DescriptorSetLayout(device, jfa_set_layout_info);

	vk::DescriptorSetLayoutBinding composite_bindings[2] = {
	        {
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        },
	        {
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        },
	};
	vk::DescriptorSetLayoutCreateInfo composite_set_layout_info{
	        .bindingCount = 2,
	        .pBindings = composite_bindings,
	};
	composite_set_layout = vk::raii::DescriptorSetLayout(device, composite_set_layout_info);

	vk::PushConstantRange jfa_push_range{
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        .offset = 0,
	        .size = sizeof(jfa_push),
	};
	vk::PipelineLayoutCreateInfo jfa_layout_info{
	        .setLayoutCount = 1,
	        .pSetLayouts = &*jfa_set_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &jfa_push_range,
	};
	jfa_layout = vk::raii::PipelineLayout(device, jfa_layout_info);

	vk::PushConstantRange composite_push_range{
	        .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        .offset = 0,
	        .size = sizeof(sdf_composite_push),
	};
	vk::PipelineLayoutCreateInfo composite_layout_info{
	        .setLayoutCount = 1,
	        .pSetLayouts = &*composite_set_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &composite_push_range,
	};
	composite_layout = vk::raii::PipelineLayout(device, composite_layout_info);

	vk::GraphicsPipelineCreateInfo jfa_pipeline_info{
	        .stageCount = 2,
	        .pStages = jfa_stages,
	        .pVertexInputState = &fs_vertex_input,
	        .pInputAssemblyState = &fs_input_assembly,
	        .pViewportState = &fs_viewport_state,
	        .pRasterizationState = &fs_rasterization,
	        .pMultisampleState = &fs_multisample,
	        .pColorBlendState = &fs_blend,
	        .pDynamicState = &fs_dynamic,
	        .layout = *jfa_layout,
	        .renderPass = *sdf_renderpass,
	};
	jfa_pipeline = vk::raii::Pipeline(device, nullptr, jfa_pipeline_info);

	// Composite targets the swapchain image: same CLEAR render pass as the
	// silhouette raster (framebuffer-compatible by construction).
	vk::GraphicsPipelineCreateInfo composite_pipeline_info{
	        .stageCount = 2,
	        .pStages = composite_stages,
	        .pVertexInputState = &fs_vertex_input,
	        .pInputAssemblyState = &fs_input_assembly,
	        .pViewportState = &fs_viewport_state,
	        .pRasterizationState = &fs_rasterization,
	        .pMultisampleState = &fs_multisample,
	        .pColorBlendState = &fs_blend,
	        .pDynamicState = &fs_dynamic,
	        .layout = *composite_layout,
	        .renderPass = *renderpass,
	};
	composite_pipeline = vk::raii::Pipeline(device, nullptr, composite_pipeline_info);

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

	vk::SamplerCreateInfo nearest_info = sampler_info;
	nearest_info.magFilter = vk::Filter::eNearest;
	nearest_info.minFilter = vk::Filter::eNearest;
	nearest_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
	nearest_sampler = vk::raii::Sampler(device, nearest_info);

	// 2 JFA sets (1 sampler each) + 2 composite sets (2 samplers each).
	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eCombinedImageSampler,
	        .descriptorCount = 6,
	};
	vk::DescriptorPoolCreateInfo pool_info{
	        .maxSets = 4,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	};
	descriptor_pool = vk::raii::DescriptorPool(device, pool_info);
	for (int eye = 0; eye < 2; ++eye)
	{
		vk::DescriptorSetAllocateInfo jfa_alloc{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = 1,
		        .pSetLayouts = &*jfa_set_layout,
		};
		jfa_sets.emplace_back(std::move(device.allocateDescriptorSets(jfa_alloc)[0]));
		vk::DescriptorSetAllocateInfo composite_alloc{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = 1,
		        .pSetLayouts = &*composite_set_layout,
		};
		composite_sets.emplace_back(std::move(device.allocateDescriptorSets(composite_alloc)[0]));
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

void feather_mask_renderer::ensure_targets(vk::Extent2D extent, int sdf_div)
{
	if (targets_extent.width == extent.width and targets_extent.height == extent.height and
	    targets_extent.width != 0 and targets_div == sdf_div)
		return;

	// Rare path (extent or divisor change): in-flight frames may still read
	// the old images, mirroring setup_reprojection_swapchain.
	device.waitIdle();
	targets_extent = extent;
	targets_div = sdf_div;
	// This group's SDF resolution (the mask extent is 64-quantized
	// upstream, hence evenly divisible by 2 and 4 throughout).
	vk::Extent2D sdf_alloc{extent.width / (uint32_t)sdf_div, extent.height / (uint32_t)sdf_div};
	for (auto * target: std::array<sdf_target *, 3>{&sdf_seed, &sdf_ping, &sdf_pong})
	{
		vk::ImageCreateInfo image_info{
		        .imageType = vk::ImageType::e2D,
		        .format = sdf_format,
		        .extent = {sdf_alloc.width, sdf_alloc.height, 1},
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
		target->fbs.clear();
		for (int layer = 0; layer < 2; ++layer)
		{
			vk::ImageViewCreateInfo view_info{
			        .image = *target->image,
			        .viewType = vk::ImageViewType::e2D,
			        .format = sdf_format,
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
		// Seed and flood passes share these framebuffers: identical
		// attachment spec (format/count/samples), which is all that
		// render-pass compatibility compares.
		vk::ImageView raw_views[2] = {*target->views[0], *target->views[1]};
		for (int layer = 0; layer < 2; ++layer)
		{
			vk::FramebufferCreateInfo fb_info{
			        .renderPass = *seed_renderpass,
			        .attachmentCount = 1,
			        .pAttachments = &raw_views[layer],
			        .width = sdf_alloc.width,
			        .height = sdf_alloc.height,
			        .layers = 1,
			};
			target->fbs.emplace_back(device, fb_info);
		}
	}
	spdlog::info("Fiducial mask SDF targets: {}x{} (1/{} of {}x{})", sdf_alloc.width, sdf_alloc.height, sdf_div, extent.width, extent.height);
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

void feather_mask_renderer::update_source(vk::ImageView view, uint32_t eye)
{
	vk::DescriptorImageInfo image_info{
	        .sampler = *nearest_sampler,
	        .imageView = view,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::WriteDescriptorSet write{
	        .dstSet = *jfa_sets[eye],
	        .dstBinding = 0,
	        .descriptorCount = 1,
	        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	        .pImageInfo = &image_info,
	};
	std::array<vk::CopyDescriptorSet, 0> no_copies{};
	device.updateDescriptorSets(write, no_copies);
}

void feather_mask_renderer::update_composite(uint32_t eye, vk::ImageView field, vk::ImageView seed)
{
	vk::DescriptorImageInfo field_info{
	        .sampler = *sampler,
	        .imageView = field,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo seed_info{
	        .sampler = *nearest_sampler,
	        .imageView = seed,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	std::array<vk::WriteDescriptorSet, 2> writes{{
	        {
	                .dstSet = *composite_sets[eye],
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &field_info,
	        },
	        {
	                .dstSet = *composite_sets[eye],
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &seed_info,
	        },
	}};
	std::array<vk::CopyDescriptorSet, 0> no_copies{};
	device.updateDescriptorSets(writes, no_copies);
}

void feather_mask_renderer::record(vk::raii::CommandBuffer & cmd,
                                   vk::Image image,
                                   vk::Extent2D extent,
                                   const std::vector<instance_draw> & draws,
                                   bool rasterize,
                                   float feather_px,
                                   const std::vector<std::array<glm::vec3, 6>> & cutouts,
                                   const std::array<glm::mat4, 2> & cutout_mvp,
                                   mask_stage_cpu * cpu_stats)
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
		section_clock raster_clk(&ms_raster);
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
		{
			fill_stats();
			return;
		}

		for (auto & [key, mesh]: meshes)
			flush_upload(cmd, mesh);

		// SDF path selection (see header): 0 = hard edge raster direct,
		// else seed + truncated Jump Flood + analytic composite at half
		// (feather <= 32) or quarter SDF resolution. Values past 128
		// clamp (documented).
		float f = feather_px;
		if (f > 128)
			f = 128;
		int sdf_div = 0;
		if (f > 0)
			sdf_div = (f > 32) ? 4 : 2;
		if (sdf_div != 0)
			ensure_targets(extent, sdf_div);

		tier_out = sdf_div;

		if (sdf_div == 0)
		{
			section_clock raster_clk(&ms_raster);
			// Hard edge: rasterize silhouettes straight into the swapchain
			// image (same pipeline, pushes and clear as the seed pass).
			// Exact, one raster pass per eye, no field work.
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
					++n_draws;
				}
				cmd.endRenderPass();
			}
		}
		else
		{
			// SDF feather path: binary seeds at reduced resolution, a Jump
			// Flood cascade truncated to the feather reach, and one analytic
			// composite into the swapchain image. Group fade opacity
			// collapses to the minimum: the field carries distance only.
			vk::Extent2D sdf_extent{extent.width / (uint32_t)sdf_div, extent.height / (uint32_t)sdf_div};
			auto set_sdf_viewport = [&](vk::Extent2D e) {
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
			vk::ClearValue seed_clear{};
			seed_clear.color.float32.fill(sdf_seed_outside);

			float min_opacity = 1;
			{
				section_clock seed_clk(&ms_raster);
				for (int eye = 0; eye < 2; ++eye)
				{
					vk::RenderPassBeginInfo begin_seed{
					        .renderPass = *seed_renderpass,
					        .framebuffer = *sdf_seed.fbs[eye],
					        .renderArea = {.offset = {0, 0}, .extent = sdf_extent},
					        .clearValueCount = 1,
					        .pClearValues = &seed_clear,
					};
					cmd.beginRenderPass(begin_seed, vk::SubpassContents::eInline);
					set_sdf_viewport(sdf_extent);
					cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *seed_pipeline);
					for (const auto & d: draws)
					{
						auto mit = meshes.find(d.mesh);
						if (mit == meshes.end() or mit->second.index_count == 0)
							continue;
						cmd.bindVertexBuffers(0, (vk::Buffer)*mit->second.vertex_buffer, (vk::DeviceSize)0);
						cmd.bindIndexBuffer(*mit->second.index_buffer, 0, vk::IndexType::eUint32);
						cmd.pushConstants<raster_push>(*pipeline_layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
						                               raster_push{.mvp = d.mvp[eye], .opacity = 1});
						cmd.drawIndexed(mit->second.index_count, 1, 0, 0, 0);
						++n_draws;
						min_opacity = std::min(min_opacity, d.opacity);
					}
					cmd.endRenderPass();
				}
			}
			make_readable(*sdf_seed.image);

			// Truncated cascade: seeds need only reach feather-px out
			// (further pixels composite to 0 regardless), so the first step
			// is the smallest power of two covering the reach in SDF texels
			// (floored at 4), then halve to 1, plus one extra step-1
			// refinement (JFA+1 against vertex errors).
			int flood_step = 4;
			while (flood_step < (int)std::ceil(f / (float)sdf_div))
				flood_step *= 2;
			sdf_target * flood_last = &sdf_ping;
			std::array<uint32_t, 0> no_offsets{};
			{
				section_clock flood_clk(&ms_blur);
				for (int eye = 0; eye < 2; ++eye)
				{
					vk::ImageView src = *sdf_seed.views[eye];
					sdf_target * dst = &sdf_ping;
					auto flood_pass = [&](int istep) {
						update_source(src, (uint32_t)eye);
						vk::RenderPassBeginInfo begin_flood{
						        .renderPass = *sdf_renderpass,
						        .framebuffer = *dst->fbs[eye],
						        .renderArea = {.offset = {0, 0}, .extent = sdf_extent},
						};
						cmd.beginRenderPass(begin_flood, vk::SubpassContents::eInline);
						set_sdf_viewport(sdf_extent);
						cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *jfa_pipeline);
						std::array<vk::DescriptorSet, 1> flood_sets{*jfa_sets[eye]};
						cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *jfa_layout, 0, flood_sets, no_offsets);
						jfa_push push{
						        .texel = {1.f / sdf_extent.width, 1.f / sdf_extent.height},
						        .step = (float)istep,
						};
						cmd.pushConstants<jfa_push>(*jfa_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
						cmd.draw(3, 1, 0, 0);
						cmd.endRenderPass();
						make_readable(*dst->image);
						flood_last = dst;
						src = *dst->views[eye];
						dst = (dst == &sdf_ping) ? &sdf_pong : &sdf_ping;
					};
					for (int s = flood_step; s >= 1; s /= 2)
						flood_pass(s);
					flood_pass(1);
				}
			}

			for (int eye = 0; eye < 2; ++eye)
			{
				update_composite((uint32_t)eye, *flood_last->views[eye], *sdf_seed.views[eye]);
				vk::RenderPassBeginInfo begin_composite{
				        .renderPass = *renderpass,
				        .framebuffer = *it->second.framebuffers[eye],
				        .renderArea = {.offset = {0, 0}, .extent = extent},
				        .clearValueCount = 1,
				        .pClearValues = &clear,
				};
				cmd.beginRenderPass(begin_composite, vk::SubpassContents::eInline);
				set_full_viewport(extent);
				cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *composite_pipeline);
				std::array<vk::DescriptorSet, 1> composite_set{*composite_sets[eye]};
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *composite_layout, 0, composite_set, no_offsets);
				sdf_composite_push push{
				        .sdf_dim = {(float)sdf_extent.width, (float)sdf_extent.height},
				        .feather_px = f,
				        .opacity = min_opacity,
				        .px_per_texel = (float)sdf_div,
				};
				cmd.pushConstants<sdf_composite_push>(*composite_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
				cmd.draw(3, 1, 0, 0);
				cmd.endRenderPass();
			}
		}
	}

	// Marker window cutouts: crisp post-composite punch. Re-begins the finished
	// swapchain framebuffers with the LOAD pass and stamps each quad at
	// FULL alpha (mask 1 = reality window): holes show passthrough, not
	// game. No SDF pass touches them, so edges stay pixel-exact. Debug
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
		section_clock punch_clk(&ms_punch);
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
