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
 * along with this program.  If not, see <https://www.gnu.org/>.
 */

#include "depth_reprojector.h"

#include "driver/configuration.h"
#include "utils/wivrn_vk_bundle.h"

#include "math/m_space.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace
{
// GL-style asymmetric frustum projection matching a standard 0..1
// nonlinear depth buffer. Column-major (GLSL mat4 order):
// v[col * 4 + row]. Returns false on degenerate input (caller falls
// back: no synthetic frame).
bool frustum_gl(const xrt_fov & fov, float near_z, float far_z, xrt_matrix_4x4 & out)
{
	if (not(std::isfinite(near_z) and std::isfinite(far_z)) or near_z <= 0 or far_z <= near_z)
		return false;
	float l = near_z * tanf(fov.angle_left);
	float r = near_z * tanf(fov.angle_right);
	float b = near_z * tanf(fov.angle_down);
	float t = near_z * tanf(fov.angle_up);
	if (not(std::isfinite(l) and std::isfinite(r) and std::isfinite(b) and std::isfinite(t)) or not(r > l and t > b))
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
        vk::SamplerAddressMode address)
{
	vk::raii::Sampler res{
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = filter,
	                .minFilter = filter,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = address,
	                .addressModeV = address,
	                .addressModeW = address,
	                // Border color only matters for ClampToBorder (the
	                // color sampler); ClampToEdge ignores it.
	                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
	        }};
	return res;
}

// Sentinel = "no splat here" (above any encodable depth).
// VULKAN_HPP_NO_CONSTRUCTORS leaves vk::ClearColorValue without any
// usable constructor, and reinterpreting the C struct trips
// -Werror=strict-aliasing: stage the bits through unsigned char
// (char aliasing is allowed) inside this one helper.
vk::ClearColorValue splat_empty_value()
{
	alignas(vk::ClearColorValue) unsigned char bits[sizeof(vk::ClearColorValue)] = {};
	VkClearColorValue raw{};
	raw.uint32[0] = 0xffffffffu;
	static_assert(sizeof(raw) == sizeof(bits), "clear layout");
	std::memcpy(bits, &raw, sizeof(bits));
	vk::ClearColorValue out;
	std::memcpy(&out, bits, sizeof(out));
	return out;
}

buffer_allocation make_warp_ubo(vk::raii::Device & device, size_t size, const char * name)
{
	return buffer_allocation{
	        device,
	        vk::BufferCreateInfo{
	                .size = size,
	                .usage = vk::BufferUsageFlagBits::eUniformBuffer,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	        },
	        name};
}

template <typename T>
void upload_ubo(buffer_allocation & ubo, const T & data)
{
	// vmaCopyMemoryToAllocation maps, copies and flushes if the heap
	// needs it: correct on coherent and non-coherent memory alike.
	vmaCopyMemoryToAllocation(vk_allocator::instance(), &data, ubo, 0, sizeof(data));
}

} // namespace

namespace wivrn
{

depth_reprojector::depth_reprojector(vk_bundle & vk, vk::Extent2D target_size) :
        reverse_z{configuration().reprojection.source_reverse_z},
        color_sampler{make_sampler(vk, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToBorder)},
        depth_sampler{make_sampler(vk, vk::Filter::eNearest, vk::SamplerAddressMode::eClampToEdge)},
        resolve_pool{nullptr},
        splat_ubo{make_warp_ubo(vk.device, sizeof(reproject_splat_ubo), "depth reprojector splat UBO 0"),
                  make_warp_ubo(vk.device, sizeof(reproject_splat_ubo), "depth reprojector splat UBO 1")},
        pull_ubo{make_warp_ubo(vk.device, sizeof(reproject_pull_ubo), "depth reprojector pull UBO 0"),
                 make_warp_ubo(vk.device, sizeof(reproject_pull_ubo), "depth reprojector pull UBO 1")}
{
	// Retained last-real-frame sources, sized to the render eye size.
	// Full-eye, GL-ordered by the resolve pass.
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
	// The source bindings (0-3) change every refresh, so this one set
	// is rewritten per real frame; everything else is written once
	// below at construction.
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

	// Per-eye warped color targets (plain UNORM, sampled raw) plus
	// per-eye splatted foreground-depth scratch (R32_UINT, cleared
	// each warp).
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
		                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		        },
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("depth reprojector target {}", i)};

		target_view[i] = make_layered_view(
		        target[i], vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, 0, 1,
		        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector target view {}", i));

		splat_depth[i] = image_allocation{
		        vk.device,
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = vk::Format::eR32Uint,
		                .extent = {target_size.width, target_size.height, 1},
		                .mipLevels = 1,
		                .arrayLayers = 1,
		                .samples = vk::SampleCountFlagBits::e1,
		                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
		        },
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("depth reprojector splat {}", i)};

		splat_view[i] = make_layered_view(
		        splat_depth[i], vk::Format::eR32Uint, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, 0, 1,
		        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
		        std::format("depth reprojector splat view {}", i));
	}

	// Splat: depth sampler + splat UBO + splat storage image.
	std::array splat_bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	splat_ds_layout = vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = splat_bindings.size(),
	                .pBindings = splat_bindings.data(),
	        }};
	vk.name(*splat_ds_layout, "depth reprojector splat descriptor set layout");

	splat_layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*splat_ds_layout,
	        }};
	vk.name(*splat_layout, "depth reprojector splat pipeline layout");

	// Pull: color sampler + pull UBO + splat storage image (read) +
	// color storage target (write).
	std::array pull_bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 3,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	pull_ds_layout = vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = pull_bindings.size(),
	                .pBindings = pull_bindings.data(),
	        }};
	vk.name(*pull_ds_layout, "depth reprojector pull descriptor set layout");

	pull_layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*pull_ds_layout,
	        }};
	vk.name(*pull_layout, "depth reprojector pull pipeline layout");

	auto splat_shader = vk.load_shader("reproject_splat");
	splat_pipeline = vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *splat_shader,
	                        .pName = "main",
	                },
	                .layout = *splat_layout,
	        }};
	vk.name(*splat_pipeline, "depth reprojector splat pipeline");

	auto pull_shader = vk.load_shader("reproject_pull");
	pull_pipeline = vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *pull_shader,
	                        .pName = "main",
	                },
	                .layout = *pull_layout,
	        }};
	vk.name(*pull_pipeline, "depth reprojector pull pipeline");

	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 4,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 4,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 6,
	        },
	};
	warp_pool = vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 4,
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        }};
	vk.name(*warp_pool, "depth reprojector warp descriptor pool");
	{
		std::array splat_layouts = {*splat_ds_layout, *splat_ds_layout};
		auto splat_sets_alloc = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *warp_pool,
		        .descriptorSetCount = uint32_t(splat_sets.size()),
		        .pSetLayouts = splat_layouts.data(),
		});
		for (int i = 0; i < 2; ++i)
			splat_sets[i] = splat_sets_alloc[i].release();
		std::array pull_layouts = {*pull_ds_layout, *pull_ds_layout};
		auto pull_sets_alloc = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *warp_pool,
		        .descriptorSetCount = uint32_t(pull_sets.size()),
		        .pSetLayouts = pull_layouts.data(),
		});
		for (int i = 0; i < 2; ++i)
			pull_sets[i] = pull_sets_alloc[i].release();
	}

	// Splat/pull descriptors reference only construction-owned views
	// and buffers: written once here, never per warp. Only the UBO
	// contents change (upload_ubo in reproject).
	for (int view = 0; view < 2; ++view)
	{
		vk::DescriptorImageInfo splat_depth_src_info{
		        .sampler = *depth_sampler,
		        .imageView = *retained_depth_eye[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorBufferInfo splat_ubo_info{
		        .buffer = splat_ubo[view],
		        .range = vk::WholeSize,
		};
		vk::DescriptorImageInfo splat_dst_info{
		        .imageView = *splat_view[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		std::array splat_writes = {
		        vk::WriteDescriptorSet{
		                .dstSet = splat_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &splat_depth_src_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = splat_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eUniformBuffer,
		                .pBufferInfo = &splat_ubo_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = splat_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &splat_dst_info,
		        },
		};
		vk.device.updateDescriptorSets(splat_writes, {});

		vk::DescriptorImageInfo pull_color_info{
		        .sampler = *color_sampler,
		        .imageView = *retained_color_eye[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorBufferInfo pull_ubo_info{
		        .buffer = pull_ubo[view],
		        .range = vk::WholeSize,
		};
		vk::DescriptorImageInfo pull_splat_info{
		        .imageView = *splat_view[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo pull_dst_info{
		        .imageView = *target_view[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		std::array pull_writes = {
		        vk::WriteDescriptorSet{
		                .dstSet = pull_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &pull_color_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = pull_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eUniformBuffer,
		                .pBufferInfo = &pull_ubo_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = pull_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &pull_splat_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = pull_sets[view],
		                .dstBinding = 3,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &pull_dst_info,
		        },
		};
		vk.device.updateDescriptorSets(pull_writes, {});
	}
}

void depth_reprojector::refresh_retained(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        std::array<vk::ImageView, 2> color,
        std::array<vk::ImageView, 2> depth,
        std::array<std::array<float, 4>, 2> color_rect,
        std::array<std::array<float, 4>, 2> depth_rect,
        bool flip_color,
        bool flip_depth)
{
	// Retained images start UNDEFINED, then stay SHADER_READ_ONLY
	// between refreshes (consumed by compute sampling in splat/pull
	// and foveation alike).
	vk::ImageLayout old_layout = retained_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined;
	vk::AccessFlags src_access = retained_initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{};
	vk::PipelineStageFlags src_stage = retained_initialized
	        ? vk::PipelineStageFlagBits::eComputeShader
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
	                .sampler = *color_sampler,
	                .imageView = color[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *color_sampler,
	                .imageView = color[1],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	};
	std::array depth_infos{
	        vk::DescriptorImageInfo{
	                .sampler = *depth_sampler,
	                .imageView = depth[0],
	                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        },
	        vk::DescriptorImageInfo{
	                .sampler = *depth_sampler,
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

	// Both flips are caller-specified: the retained images stay
	// GL-ordered (row 0 = frustum bottom) regardless of source order.
	reproject_resolve_push push{};
	std::ranges::copy(color_rect[0], push.color_rect[0]);
	std::ranges::copy(color_rect[1], push.color_rect[1]);
	std::ranges::copy(depth_rect[0], push.depth_rect[0]);
	std::ranges::copy(depth_rect[1], push.depth_rect[1]);
	push.color_flip = flip_color ? 1.f : 0.f;
	push.depth_flip = flip_depth ? 1.f : 0.f;
	// Reverse-Z sources convert once here so the warp math stays
	// standard-convention downstream.
	push.src_reverse_z = reverse_z ? 1.f : 0.f;
	push.pad = 0.f;

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
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        out_barriers);
}

bool depth_reprojector::reproject(
        vk::raii::CommandBuffer & cmd,
        int view,
        const xrt_pose & src_pose,
        const xrt_fov & src_fov,
        float near_z,
        float far_z,
        const xrt_pose & dst_pose,
        const xrt_fov & dst_fov)
{
	// Source clip math in GL convention (matches the 0..1 depth buffer,
	// reverse-Z already converted by the resolve pass).
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

	xrt_matrix_4x4 dst_inv_p{};
	math_matrix_4x4_inverse(&dst_proj, &dst_inv_p);
	xrt_matrix_4x4 dst_inv_v{};
	math_matrix_4x4_inverse(&dst_view, &dst_inv_v);

	reproject_splat_ubo splat_data{};
	math_matrix_4x4_multiply(&render_inv_v, &render_inv_p, &splat_data.render_inv_vp);
	splat_data.warp_vp = warp_vp;
	upload_ubo(splat_ubo[view], splat_data);

	reproject_pull_ubo pull_data{};
	math_matrix_4x4_multiply(&dst_inv_v, &dst_inv_p, &pull_data.dst_inv);
	math_matrix_4x4_multiply(&src_proj, &src_view, &pull_data.src_vp);
	pull_data.hole_radius = float(kHoleRadius);
	pull_data.pad[0] = pull_data.pad[1] = pull_data.pad[2] = 0;
	upload_ubo(pull_ubo[view], pull_data);

	const auto extent = target[view].info().extent;

	// Splat scratch ends each warp in GENERAL (pull only reads); the
	// target ends each warp in SHADER_READ_ONLY (foveation only
	// samples), so the next warp reuses it from there. First warp from
	// UNDEFINED.
	vk::ImageLayout splat_start =
	        warp_targets_ready ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined;
	vk::AccessFlags splat_src_access =
	        warp_targets_ready ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite : vk::AccessFlags{};
	vk::ImageLayout target_start =
	        warp_targets_ready ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined;
	vk::AccessFlags target_src_access =
	        warp_targets_ready ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{};
	vk::PipelineStageFlags reuse_stage =
	        warp_targets_ready ? vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eTopOfPipe;
	warp_targets_ready = true;

	std::array pre_barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = splat_src_access,
	                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
	                .oldLayout = splat_start,
	                .newLayout = vk::ImageLayout::eTransferDstOptimal,
	                .image = splat_depth[view],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = target_src_access,
	                .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .oldLayout = target_start,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = target[view],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};
	cmd.pipelineBarrier(
	        reuse_stage,
	        vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        pre_barriers);

	cmd.clearColorImage(
	        splat_depth[view],
	        vk::ImageLayout::eTransferDstOptimal,
	        splat_empty_value(),
	        vk::ImageSubresourceRange{
	                .aspectMask = vk::ImageAspectFlagBits::eColor,
	                .levelCount = 1,
	                .layerCount = 1,
	        });

	std::array clear_barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = splat_depth[view],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eTransfer,
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        clear_barriers);

	// Splat/pull sets were written once at construction; only the UBO
	// contents above changed.
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *splat_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *splat_layout, 0, splat_sets[view], {});
	cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

	// Splat writes (atomics) before pull reads, same queue.
	std::array mid_barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = splat_depth[view],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eComputeShader,
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        mid_barriers);

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pull_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pull_layout, 0, pull_sets[view], {});
	cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

	// Target ends SHADER_READ_ONLY_OPTIMAL, ready to feed the compute
	// sampling in foveation. Splat scratch stays GENERAL for the next
	// warp's clear.
	std::array out_barriers{
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eGeneral,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = target[view],
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = 1,
	                },
	        },
	};
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eComputeShader,
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        out_barriers);

	return true;
}

std::array<vk::ImageView, 2> depth_reprojector::get_views()
{
	return {*target_view[0], *target_view[1]};
}

} // namespace wivrn
