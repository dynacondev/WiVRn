/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "depth_reprojector.h"

#include "driver/configuration.h"
#include "utils/wivrn_vk_bundle.h"

#include "math/m_space.h"
#include "util/u_logging.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace
{
// GL-style asymmetric frustum projection matching a standard 0..1
// nonlinear depth buffer (X-Plane via xrizer: D32_SFLOAT, near/far from
// the depth layer). Column-major (GLSL mat4 order): v[col * 4 + row].
// Returns false on degenerate input (caller falls back).
bool frustum_gl(const xrt_fov & fov, float near_z, float far_z, xrt_matrix_4x4 & out)
{
	if (not(std::isfinite(near_z) and std::isfinite(far_z)) or near_z <= 0 or far_z <= near_z)
		return false;
	float l = near_z * tanf(fov.angle_left);
	float r = near_z * tanf(fov.angle_right);
	float b = near_z * tanf(fov.angle_down);
	float t = near_z * tanf(fov.angle_up);
	if (not(r > l and t > b))
		return false;
	float rl = 2 * near_z / (r - l);
	float tb = 2 * near_z / (t - b);
	float rl_mid = (r + l) / (r - l);
	float tb_mid = (t + b) / (t - b);
	float fn = -(far_z + near_z) / (far_z - near_z);
	float fn2 = -2 * far_z * near_z / (far_z - near_z);
	// Column 0
	out.v[0] = rl;
	out.v[1] = 0;
	out.v[2] = 0;
	out.v[3] = 0;
	// Column 1
	out.v[4] = 0;
	out.v[5] = tb;
	out.v[6] = 0;
	out.v[7] = 0;
	// Column 2
	out.v[8] = rl_mid;
	out.v[9] = tb_mid;
	out.v[10] = fn;
	out.v[11] = -1;
	// Column 3
	out.v[12] = 0;
	out.v[13] = 0;
	out.v[14] = fn2;
	out.v[15] = 0;
	return true;
}

vk::raii::Sampler make_sampler(
        wivrn::vk_bundle & vk,
        vk::Filter filter,
        vk::SamplerAddressMode address,
        vk::BorderColor border)
{
	vk::raii::Sampler res{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = filter,
	                .minFilter = filter,
	                .mipmapMode = vk::SamplerMipmapMode::eLinear,
	                .addressModeU = address,
	                .addressModeV = address,
	                .addressModeW = address,
	                .borderColor = border,
	        }};
	vk.name(*res, "depth reprojector sampler");
	return res;
}

buffer_allocation make_ubo(vk::raii::Device & device, int i)
{
	return buffer_allocation{
	        device,
	        vk::BufferCreateInfo{
	                .size = sizeof(reproject_ubo_data),
	                .usage = vk::BufferUsageFlagBits::eUniformBuffer,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO,
	        },
	        std::format("depth reprojector UBO {}", i)};
}

uint32_t mesh_dim(uint32_t configured, uint32_t automatic)
{
	if (configured != 0)
		return std::clamp(configured, 16u, 4096u);
	// Automatic: quarter of the target resolution, clamped. Full-res mesh
	// (e.g. 2476x2594 = 6.4M quads = 154MB index buffer) burns VRAM and
	// vertex throughput for no visual gain — depth varies smoothly, and
	// OpenWarp itself defaults to ~1k meshes for 2k targets.
	if (automatic == 0)
		return 256;
	uint32_t q = (automatic + 3) / 4;
	return std::clamp(q, 16u, 512u);
}

} // namespace

namespace wivrn
{

depth_reprojector::depth_reprojector(vk_bundle & vk, vk::Extent2D target_size) :
        graphics_supported{
                bool(vk.physical_device.getQueueFamilyProperties()[vk.queue.family_index].queueFlags &
                     vk::QueueFlagBits::eGraphics)},
        mesh_w{mesh_dim(configuration().reprojection.mesh_width, target_size.width)},
        mesh_h{mesh_dim(configuration().reprojection.mesh_height, target_size.height)},
        bleed_radius{std::max(configuration().reprojection.bleed_radius, 0.f)},
        bleed_tolerance{std::max(configuration().reprojection.bleed_tolerance, 0.f)},
        clamp_to_border_black{make_sampler(vk, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToBorder, vk::BorderColor::eFloatOpaqueBlack)},
        clamp_to_edge{make_sampler(vk, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge, vk::BorderColor::eFloatOpaqueBlack)},
        resolve_pool{nullptr},
        ubo{make_ubo(vk.device, 0), make_ubo(vk.device, 1)}
{
	if (not graphics_supported)
	{
		U_LOG_W("Depth reprojection disabled: compositor queue family has no graphics support");
		return;
	}

	// Retained last-real-frame sources, sized to the render (== depth)
	// eye size. Full-eye, GL-ordered by the resolve pass.
	retained_color = image_allocation{
	        vk.device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = vk::Format::eR8G8B8A8Unorm,
	                .extent = {target_size.width, target_size.height, 1},
	                .mipLevels = 1,
	                .arrayLayers = 2,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        "depth reprojector retained color"};
	retained_depth = image_allocation{
	        vk.device,
	        vk::ImageCreateInfo{
	                .imageType = vk::ImageType::e2D,
	                .format = vk::Format::eR32Sfloat,
	                .extent = {target_size.width, target_size.height, 1},
	                .mipLevels = 1,
	                .arrayLayers = 2,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
	        },
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        "depth reprojector retained depth"};

	auto make_layered_view = [&](vk::Image image, vk::Format format, vk::ImageAspectFlags aspect,
	                             vk::ImageViewType type, uint32_t base_layer, uint32_t layers,
	                             vk::ImageUsageFlags usage, const std::string & name) {
		vk::raii::ImageView view{
		        vk.device,
		        vk::StructureChain{
		                vk::ImageViewCreateInfo{
		                        .image = image,
		                        .viewType = type,
		                        .format = format,
		                        .subresourceRange = {
		                                .aspectMask = aspect,
		                                .levelCount = 1,
		                                .baseArrayLayer = base_layer,
		                                .layerCount = layers,
		                        },
		                },
		                vk::ImageViewUsageCreateInfo{.usage = usage},
		        }
		                .get()};
		vk.name(*view, name.c_str());
		return view;
	};

	retained_color_view = make_layered_view(
	        retained_color, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
	        vk::ImageViewType::e2DArray, 0, 2,
	        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, "depth reprojector retained color array");
	retained_depth_view = make_layered_view(
	        retained_depth, vk::Format::eR32Sfloat, vk::ImageAspectFlagBits::eColor,
	        vk::ImageViewType::e2DArray, 0, 2,
	        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled, "depth reprojector retained depth array");
	for (int i = 0; i < 2; ++i)
	{
		retained_color_eye[i] = make_layered_view(
		        retained_color, vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, i, 1, vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector retained color {}", i));
		retained_depth_eye[i] = make_layered_view(
		        retained_depth, vk::Format::eR32Sfloat, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, i, 1, vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector retained depth {}", i));
	}

	// Resolve pass: compute copy of the current sources into retained.
	std::array resolve_bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 3,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 4,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 5,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	resolve_ds_layout = vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = resolve_bindings.size(),
	                .pBindings = resolve_bindings.data(),
	        }};
	vk.name(*resolve_ds_layout, "depth reprojector resolve descriptor set layout");

	vk::PushConstantRange resolve_push{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(reproject_resolve_push),
	};
	resolve_layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*resolve_ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &resolve_push,
	        }};
	vk.name(*resolve_layout, "depth reprojector resolve pipeline layout");

	auto resolve_shader = vk.load_shader("reproject_resolve");
	resolve_pipeline = vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *resolve_shader,
	                        .pName = "main",
	                },
	                .layout = *resolve_layout,
	        }};
	vk.name(*resolve_pipeline, "depth reprojector resolve pipeline");

	std::array resolve_pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 4,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 2,
	        },
	};
	resolve_pool = vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 1,
	                .poolSizeCount = resolve_pool_sizes.size(),
	                .pPoolSizes = resolve_pool_sizes.data(),
	        }};
	vk.name(*resolve_pool, "depth reprojector resolve descriptor pool");
	resolve_set = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		.descriptorPool = *resolve_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &*resolve_ds_layout,
	})[0]
	                        .release();

	// Per-eye warped color targets (plain UNORM, sampled raw).
	for (int i = 0; i < 2; ++i)
	{
		target[i] = image_allocation{
		        vk.device,
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = vk::Format::eR8G8B8A8Unorm,
		                .extent = {target_size.width, target_size.height, 1},
		                .mipLevels = 1,
		                .arrayLayers = 1,
		                .samples = vk::SampleCountFlagBits::e1,
		                .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		        },
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("depth reprojector target {}", i)};

		target_view[i] = make_layered_view(
		        target[i], vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, 0, 1,
		        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector target view {}", i));
	}

	// Renderpass: clear holes to transparent black, store, end sampled.
	vk::AttachmentDescription attachment{
	        .format = vk::Format::eR8G8B8A8Unorm,
	        .samples = vk::SampleCountFlagBits::e1,
	        .loadOp = vk::AttachmentLoadOp::eClear,
	        .storeOp = vk::AttachmentStoreOp::eStore,
	        .initialLayout = vk::ImageLayout::eUndefined,
	        .finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
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
	renderpass = vk::raii::RenderPass{
	        vk.device,
	        vk::RenderPassCreateInfo{
	                .attachmentCount = 1,
	                .pAttachments = &attachment,
	                .subpassCount = 1,
	                .pSubpasses = &subpass,
	        }};
	vk.name(*renderpass, "depth reprojector renderpass");

	for (int i = 0; i < 2; ++i)
	{
		framebuffers[i] = vk::raii::Framebuffer{
		        vk.device,
		        vk::FramebufferCreateInfo{
		                .renderPass = *renderpass,
		                .attachmentCount = 1,
		                .pAttachments = &*target_view[i],
		                .width = target_size.width,
		                .height = target_size.height,
		                .layers = 1,
		        }};
		vk.name(*framebuffers[i], std::format("depth reprojector framebuffer {}", i).c_str());
	}

	// Descriptor layout: color (fragment), depth (vertex), UBO (vertex).
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eFragment,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eVertex,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eVertex,
	        },
	};
	ds_layout = vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        }};
	vk.name(*ds_layout, "depth reprojector descriptor set layout");

	vk::PushConstantRange push_range{
	        .stageFlags = vk::ShaderStageFlagBits::eVertex,
	        .offset = 0,
	        .size = sizeof(std::array<uint32_t, 2>),
	};
	layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &push_range,
	        }};
	vk.name(*layout, "depth reprojector pipeline layout");

	auto vert = vk.load_shader("reproject_mesh.vert");
	auto frag = vk.load_shader("reproject_mesh.frag");

	vk::PipelineVertexInputStateCreateInfo vertex_input{};
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
	// No blending: forward-warped fragments win as rasterized, holes
	// keep the clear color.
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
	vk::PipelineShaderStageCreateInfo stages[2] = {
	        {
	                .stage = vk::ShaderStageFlagBits::eVertex,
	                .module = *vert,
	                .pName = "main",
	        },
	        {
	                .stage = vk::ShaderStageFlagBits::eFragment,
	                .module = *frag,
	                .pName = "main",
	        },
	};
	pipeline = vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::GraphicsPipelineCreateInfo{
	                .stageCount = 2,
	                .pStages = stages,
	                .pVertexInputState = &vertex_input,
	                .pInputAssemblyState = &input_assembly,
	                .pViewportState = &viewport_state,
	                .pRasterizationState = &rasterization,
	                .pMultisampleState = &multisample,
	                .pColorBlendState = &blend,
	                .pDynamicState = &dynamic,
	                .layout = *layout,
	                .renderPass = *renderpass,
	        }};
	vk.name(*pipeline, "depth reprojector pipeline");

	// Shared grid index buffer for both eyes: W*H quads as triangles.
	// Host-visible + coherent only: requiring DEVICE_LOCAL here fails on
	// discrete GPUs (BAR host-visible is rarely device-local, and the
	// combined flag set often has no heap at all -> allocation failure).
	const uint64_t quad_count = uint64_t(mesh_w) * uint64_t(mesh_h);
	index_buffer = buffer_allocation{
	        vk.device,
	        vk::BufferCreateInfo{
	                .size = quad_count * 6 * sizeof(uint32_t),
	                .usage = vk::BufferUsageFlagBits::eIndexBuffer,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO,
	        },
	        "depth reprojector index buffer"};
	{
		uint32_t * idx = index_buffer.data<uint32_t>();
		uint32_t cols = mesh_w + 1;
		for (uint32_t y = 0; y < mesh_h; ++y)
		{
			for (uint32_t x = 0; x < mesh_w; ++x)
			{
				uint32_t v0 = y * cols + x;
				*idx++ = v0;
				*idx++ = v0 + 1;
				*idx++ = v0 + cols;
				*idx++ = v0 + 1;
				*idx++ = v0 + cols + 1;
				*idx++ = v0 + cols;
			}
		}
	}

	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 4,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 2,
	        },
	};
	descriptor_pool = vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 2,
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        }};
	vk.name(*descriptor_pool, "depth reprojector pipeline descriptor pool");
	{
		std::array layouts = {*ds_layout, *ds_layout};
		auto sets = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *descriptor_pool,
		        .descriptorSetCount = uint32_t(descriptor_sets.size()),
		        .pSetLayouts = layouts.data(),
		});
		for (int i = 0; i < 2; ++i)
			descriptor_sets[i] = sets[i].release();
	}
}

void depth_reprojector::refresh_retained(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        std::array<vk::ImageView, 2> color,
        std::array<vk::ImageView, 2> depth,
        std::array<std::array<float, 4>, 2> color_rect,
        std::array<std::array<float, 4>, 2> depth_rect,
        bool flip_y)
{
	// Retained images start UNDEFINED, then stay SHADER_READ_ONLY
	// between refreshes.
	vk::ImageLayout old_layout = retained_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined;
	vk::AccessFlags src_access = retained_initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{};
	vk::PipelineStageFlags src_stage = retained_initialized
	        ? vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader
	        : vk::PipelineStageFlagBits::eTopOfPipe;
	retained_initialized = true;

	std::array barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = src_access,
	                .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .oldLayout = old_layout,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = retained_color,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = src_access,
	                .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .oldLayout = old_layout,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = retained_depth,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	};
	cmd.pipelineBarrier(src_stage, vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, barriers);

	std::array color_infos{
	        vk::DescriptorImageInfo{
	                .sampler = *clamp_to_border_black,
	                .imageView = color[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *clamp_to_border_black,
	                .imageView = color[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};
	std::array depth_infos{
	        vk::DescriptorImageInfo{
	                .sampler = *clamp_to_edge,
	                .imageView = depth[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *clamp_to_edge,
	                .imageView = depth[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};
	vk::DescriptorImageInfo dst_color_info{
	        .imageView = *retained_color_view,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo dst_depth_info{
	        .imageView = *retained_depth_view,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	std::array writes = {
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &color_infos[0],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &color_infos[1],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &depth_infos[0],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 3,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &depth_infos[1],
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 4,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &dst_color_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = resolve_set,
	                .dstBinding = 5,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .pImageInfo = &dst_depth_info,
	        },
	};
	device.updateDescriptorSets(writes, {});

	// GL-ordered sources (flip_y) need no mirroring; Vulkan-ordered
	// rows sample mirrored so retained stays GL-ordered.
	reproject_resolve_push push{};
	std::ranges::copy(color_rect[0], push.color_rect[0]);
	std::ranges::copy(color_rect[1], push.color_rect[1]);
	std::ranges::copy(depth_rect[0], push.depth_rect[0]);
	std::ranges::copy(depth_rect[1], push.depth_rect[1]);
	push.src_flip = flip_y ? 0.f : 1.f;

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *resolve_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *resolve_layout, 0, resolve_set, {});
	cmd.pushConstants<reproject_resolve_push>(*resolve_layout, vk::ShaderStageFlagBits::eCompute, 0, push);
	const auto extent = retained_color.info().extent;
	cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 2);

	std::array out_barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = retained_color,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = retained_depth,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 2,
	                },
	        },
	};
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eComputeShader,
	        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
	        {},
	        {},
	        {},
	        out_barriers);
}

bool depth_reprojector::reproject(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        int view,
        const xrt_pose & src_pose,
        const xrt_fov & src_fov,
        float near_z,
        float far_z,
        const xrt_pose & dst_pose,
        const xrt_fov & dst_fov)
{
	// Source clip math in GL convention (matches the 0..1 depth buffer).
	xrt_matrix_4x4 src_proj{};
	if (not frustum_gl(src_fov, near_z, far_z, src_proj))
		return false;
	xrt_matrix_4x4 src_view{};
	math_matrix_4x4_view_from_pose(&src_pose, &src_view);
	xrt_matrix_4x4 render_inv_p{};
	math_matrix_4x4_inverse(&src_proj, &render_inv_p);
	xrt_matrix_4x4 render_inv_v{};
	math_matrix_4x4_inverse(&src_view, &render_inv_v);

	xrt_matrix_4x4 dst_proj{};
	if (not frustum_gl(dst_fov, near_z, far_z, dst_proj))
		return false;
	xrt_matrix_4x4 dst_view{};
	math_matrix_4x4_view_from_pose(&dst_pose, &dst_view);
	xrt_matrix_4x4 warp_vp{};
	math_matrix_4x4_multiply(&dst_proj, &dst_view, &warp_vp);

	auto & ubo_data = *(reproject_ubo_data *)ubo[view].map();
	ubo_data.render_inv_p = render_inv_p;
	ubo_data.render_inv_v = render_inv_v;
	ubo_data.warp_vp = warp_vp;
	// Retained sources are full-eye by construction.
	ubo_data.color_rect[0] = ubo_data.depth_rect[0] = 0;
	ubo_data.color_rect[1] = ubo_data.depth_rect[1] = 0;
	ubo_data.color_rect[2] = ubo_data.depth_rect[2] = 1;
	ubo_data.color_rect[3] = ubo_data.depth_rect[3] = 1;
	ubo_data.bleed_radius = bleed_radius;
	ubo_data.bleed_tolerance = bleed_tolerance;
	ubo_data.pad[0] = ubo_data.pad[1] = 0;

	const auto extent = target[view].info().extent;
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);

	vk::DescriptorImageInfo color_info{
	        .sampler = *clamp_to_border_black,
	        .imageView = *retained_color_eye[view],
	        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo depth_info{
	        .sampler = *clamp_to_edge,
	        .imageView = *retained_depth_eye[view],
	        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorBufferInfo ubo_info{
	        .buffer = ubo[view],
	        .range = vk::WholeSize,
	};
	std::array writes = {
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_sets[view],
	                .dstBinding = 0,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &color_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_sets[view],
	                .dstBinding = 1,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .pImageInfo = &depth_info,
	        },
	        vk::WriteDescriptorSet{
	                .dstSet = descriptor_sets[view],
	                .dstBinding = 2,
	                .descriptorCount = 1,
	                .descriptorType = vk::DescriptorType::eUniformBuffer,
	                .pBufferInfo = &ubo_info,
	        },
	};
	device.updateDescriptorSets(writes, {});

	// Target rows are GL-ordered (row 0 = frustum bottom), matching the
	// retained source convention the mesh math assumes.
	vk::Viewport viewport{
	        .x = 0,
	        .y = 0,
	        .width = float(extent.width),
	        .height = float(extent.height),
	        .minDepth = 0,
	        .maxDepth = 1,
	};
	vk::Rect2D scissor{.extent = {extent.width, extent.height}};
	cmd.setViewport(0, viewport);
	cmd.setScissor(0, scissor);

	vk::ClearValue clear{};
	cmd.beginRenderPass(
	        vk::RenderPassBeginInfo{
	                .renderPass = *renderpass,
	                .framebuffer = *framebuffers[view],
	                .renderArea = {.extent = {extent.width, extent.height}},
	                .clearValueCount = 1,
	                .pClearValues = &clear,
	        },
	        vk::SubpassContents::eInline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, descriptor_sets[view], {});
	std::array<uint32_t, 2> grid{mesh_w, mesh_h};
	cmd.pushConstants<std::array<uint32_t, 2>>(*layout, vk::ShaderStageFlagBits::eVertex, 0, grid);
	cmd.bindIndexBuffer(index_buffer, 0, vk::IndexType::eUint32);
	cmd.drawIndexed(mesh_w * mesh_h * 6, 1, 0, 0, 0);
	cmd.endRenderPass();

	// Renderpass ends SHADER_READ_ONLY (finalLayout); add the execution +
	// memory dependency for the compute sampling in foveation.
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eColorAttachmentOutput,
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        vk::MemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	        },
	        {},
	        {});

	return true;
}

std::array<vk::ImageView, 2> depth_reprojector::get_views()
{
	return {*target_view[0], *target_view[1]};
}

} // namespace wivrn
