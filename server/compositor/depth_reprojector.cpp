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

// Monado includes
#include "util/u_logging.h"

#include <algorithm>
#include <cassert>
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
	// GL internal convention (NDC z in [-1,1], remapped *0.5+0.5 for
	// storage) reproduces exactly the depth values a Vulkan app stores:
	// Vulkan's [0,1] NDC was defined to coincide with the remapped GL
	// values (both are f/(f-n) - nf/((f-n)*d)). So this matrix matches
	// Vulkan-rendered depth buffers; no convention conversion needed.
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
        vk::SamplerAddressMode address,
        float max_lod = 0.0f)
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
	                // maxLod must be uncapped on samplers used for
	                // explicit textureLod taps (the fill pass samples
	                // mip levels; the default 0.0 would clamp every
	                // tap to level 0).
	                .minLod = 0.0f,
	                .maxLod = max_lod,
	                // Border color only matters for ClampToBorder;
	                // ClampToEdge ignores it.
	                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
	        }};
	return res;
}

// Sentinel bytes = "no splat here" (above any encodable depth).
// VULKAN_HPP_NO_CONSTRUCTORS leaves vk::ClearColorValue without any
// usable constructor, and reinterpreting the C struct trips
// -Werror=strict-aliasing: stage the bits through unsigned char (char
// aliasing is allowed) and reinterpret at the call site, which takes
// the value by const reference (never materialized as the union type).
// (VkClearColorValue <-> vk::ClearColorValue layout identity is
// static_asserted inside vulkan.hpp itself.)
struct splat_empty_storage
{
	alignas(VkClearColorValue) unsigned char bytes[sizeof(VkClearColorValue)];
};
splat_empty_storage splat_empty_bytes()
{
	splat_empty_storage out{};
	VkClearColorValue raw{};
	raw.uint32[0] = 0xffffffffu;
	static_assert(sizeof(raw) == sizeof(out.bytes), "clear layout");
	std::memcpy(out.bytes, &raw, sizeof(out.bytes));
	return out;
}

bool blit_supported_format(const wivrn::vk_bundle & vk)
{
	vk::FormatProperties props =
	        vk.physical_device.getFormatProperties(vk::Format::eR8G8B8A8Unorm);
	constexpr auto need = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
	return (props.optimalTilingFeatures & need) == need;
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

// TEMPORARY triage switches for a GPU hang bisect (remove once the
// fill path is proven on the failing driver). kForceDisableBlits kills
// the whole blit chain + fill pass (holes stay black). kSkipFillDispatch
// keeps the blits running but skips only the fill dispatch, to split a
// blit-chain fault from a fill-dispatch fault. kSkipBlitImage keeps all
// the mip barriers but skips the blitImage calls themselves, to split
// barrier/layout handling from the blit itself.
static constexpr bool kForceDisableBlits = false;
static constexpr bool kSkipFillDispatch = true;
static constexpr bool kSkipBlitImage = true;
// WV-B1 coupling: the fill pass samples mip levels 1..N built by the blit
// chain, so fill-without-blits (kSkipBlitImage && !kSkipFillDispatch) would
// sample UNDEFINED/STALE mips. The fill dispatch below therefore conjoins
// !kSkipBlitImage (fill requires blits); this assert makes the hazardous
// combo unrepresentable at compile time. Current disabled behavior is
// unchanged (both still skipped).
static_assert(!kSkipBlitImage || kSkipFillDispatch,
              "WV-B1: fill requires blits (kSkipBlitImage => kSkipFillDispatch)");

depth_reprojector::depth_reprojector(vk_bundle & vk, vk::Extent2D target_size) :
        reverse_z{configuration().reprojection.source_reverse_z},
        blit_supported{blit_supported_format(vk) && !kForceDisableBlits},
        // Color sampler: linear, edge-clamped, uncapped LOD. Out-of-bounds
        // taps are rejected in-shader, so border-black would only darken
        // the valid rim; the fill pass needs explicit mip LODs.
        color_sampler{make_sampler(vk, vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge,
                                  VK_LOD_CLAMP_NONE)},
        depth_sampler{make_sampler(vk, vk::Filter::eNearest, vk::SamplerAddressMode::eClampToEdge)},
        resolve_pool{nullptr},
        splat_ubo{make_warp_ubo(vk.device, sizeof(reproject_splat_ubo), "depth reprojector splat UBO 0"),
                  make_warp_ubo(vk.device, sizeof(reproject_splat_ubo), "depth reprojector splat UBO 1")},
        pull_ubo{make_warp_ubo(vk.device, sizeof(reproject_pull_ubo), "depth reprojector pull UBO 0"),
                 make_warp_ubo(vk.device, sizeof(reproject_pull_ubo), "depth reprojector pull UBO 1")}
{
	if (!blit_supported)
		U_LOG_W("depth warp: R8G8B8A8 blits unsupported, hole fill disabled (holes stay black)");
	if (kForceDisableBlits)
		U_LOG_W("depth warp: hole fill FORCE-DISABLED (temporary triage, remove me)");
	if (!kForceDisableBlits && kSkipFillDispatch)
		U_LOG_W("depth warp: fill dispatch SKIPPED, blits running (temporary triage, remove me)");
	if (!kForceDisableBlits && kSkipBlitImage)
		U_LOG_W("depth warp: blitImage calls SKIPPED, barriers running (temporary triage, remove me)");
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
	                             vk::ImageUsageFlags usage, const std::string & name,
	                             uint32_t base_level = 0, uint32_t levels = 1) {
		vk::raii::ImageView view{
		        vk.device,
		        vk::StructureChain{
		                vk::ImageViewCreateInfo{
		                        .image = image,
		                        .viewType = type,
		                        .format = format,
		                        .subresourceRange = {
		                                .aspectMask = aspect,
		                                .baseMipLevel = base_level,
		                                .levelCount = levels,
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
	// each warp). Targets carry a full mip chain for the fill pass
	// (blits read level 0, fill samples levels 1..N).
	mip_levels = 1;
	for (uint32_t m = std::max(target_size.width, target_size.height); m > 1; m >>= 1)
		mip_levels++;
	// Floor at 2: the fill view needs levels 1..N to exist, and the push
	// constant LOD cap goes negative otherwise. Real eye sizes give ~11.
	mip_levels = std::max(2u, mip_levels);
	for (int i = 0; i < 2; ++i)
	{
		target[i] = image_allocation{
		        vk.device,
		        vk::ImageCreateInfo{
		                .imageType = vk::ImageType::e2D,
		                .format = vk::Format::eR8G8B8A8Unorm,
		                .extent = {target_size.width, target_size.height, 1},
		                .mipLevels = mip_levels,
		                .arrayLayers = 1,
		                .samples = vk::SampleCountFlagBits::e1,
		                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
		                vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
		        },
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("depth reprojector target {}", i)};

		target_view[i] = make_layered_view(
		        target[i], vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, 0, 1,
		        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector target view {}", i), 0, 1);

		// Levels 1..N sampled by the fill pass (level 0 is written
		// while these are read, so it must not be in this view).
		target_mip_view[i] = make_layered_view(
		        target[i], vk::Format::eR8G8B8A8Unorm, vk::ImageAspectFlagBits::eColor,
		        vk::ImageViewType::e2D, 0, 1, vk::ImageUsageFlagBits::eSampled,
		        std::format("depth reprojector target mips {}", i), 1, mip_levels - 1);

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

	vk::PushConstantRange splat_push{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(reproject_splat_push),
	};
	splat_layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*splat_ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &splat_push,
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

	// Fill: dst storage image (read-write) + splat storage image
	// (read) + dst mip chain sampled (levels 1..N). Push constant
	// carries the view-relative max LOD.
	std::array fill_bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	fill_ds_layout = vk::raii::DescriptorSetLayout{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = fill_bindings.size(),
	                .pBindings = fill_bindings.data(),
	        }};
	vk.name(*fill_ds_layout, "depth reprojector fill descriptor set layout");

	vk::PushConstantRange fill_push{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(int32_t),
	};
	fill_layout = vk::raii::PipelineLayout{
	        vk.device,
	        vk::PipelineLayoutCreateInfo{
	                .setLayoutCount = 1,
	                .pSetLayouts = &*fill_ds_layout,
	                .pushConstantRangeCount = 1,
	                .pPushConstantRanges = &fill_push,
	        }};
	vk.name(*fill_layout, "depth reprojector fill pipeline layout");

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

	auto fill_shader = vk.load_shader("reproject_fill");
	fill_pipeline = vk::raii::Pipeline{
	        vk.device,
	        nullptr,
	        vk::ComputePipelineCreateInfo{
	                .stage = {
	                        .stage = vk::ShaderStageFlagBits::eCompute,
	                        .module = *fill_shader,
	                        .pName = "main",
	                },
	                .layout = *fill_layout,
	        }};
	vk.name(*fill_pipeline, "depth reprojector fill pipeline");

	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 6,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eUniformBuffer,
	                .descriptorCount = 4,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 10,
	        },
	};
	warp_pool = vk::raii::DescriptorPool{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 6,
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
		std::array fill_layouts = {*fill_ds_layout, *fill_ds_layout};
		auto fill_sets_alloc = vk.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
		        .descriptorPool = *warp_pool,
		        .descriptorSetCount = uint32_t(fill_sets.size()),
		        .pSetLayouts = fill_layouts.data(),
		});
		for (int i = 0; i < 2; ++i)
			fill_sets[i] = fill_sets_alloc[i].release();
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

		// Fill descriptors reference only construction-owned views:
		// dst storage (read-write) + splat storage (read) + dst
		// mip-chain sampled (levels 1..N, shader-read-only after the
		// blit chain builds them).
		vk::DescriptorImageInfo fill_dst_info{
		        .imageView = *target_view[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo fill_splat_info{
		        .imageView = *splat_view[view],
		        .imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo fill_mips_info{
		        .sampler = *color_sampler,
		        .imageView = *target_mip_view[view],
		        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		std::array fill_writes = {
		        vk::WriteDescriptorSet{
		                .dstSet = fill_sets[view],
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &fill_dst_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = fill_sets[view],
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eStorageImage,
		                .pImageInfo = &fill_splat_info,
		        },
		        vk::WriteDescriptorSet{
		                .dstSet = fill_sets[view],
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		                .pImageInfo = &fill_mips_info,
		        },
		};
		vk.device.updateDescriptorSets(fill_writes, {});
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
	upload_ubo(pull_ubo[view], pull_data);

	const auto extent = target[view].info().extent;

	// Splat scratch ends each warp in GENERAL (pull only reads); the
	// target ends each warp in SHADER_READ_ONLY (foveation only
	// samples), so the next warp reuses it from there. First warp per
	// view from UNDEFINED. Per-view: the two eyes warp independently
	// (view 0 must not mark view 1's resources transitioned).
	vk::ImageLayout splat_start =
	        warp_targets_ready[view] ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined;
	vk::AccessFlags splat_src_access =
	        warp_targets_ready[view] ? vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite : vk::AccessFlags{};
	vk::ImageLayout target_start =
	        warp_targets_ready[view] ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined;
	vk::AccessFlags target_src_access =
	        warp_targets_ready[view] ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{};
	vk::PipelineStageFlags reuse_stage =
	        warp_targets_ready[view] ? vk::PipelineStageFlagBits::eComputeShader : vk::PipelineStageFlagBits::eTopOfPipe;
	// Mip levels 1..N persist SHADER_READ_ONLY across warps (only
	// level 0 cycles); remember whether this is the first warp so
	// the blit chain below picks the right old layouts.
	const bool first_warp = !warp_targets_ready[view];
	warp_targets_ready[view] = true;

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
	// WV-R11: warp_targets_ready[view] is the only first/repeat
	// distinguisher for these layouts (set true just above; first_warp
	// captured before). Assert the expected level-0 start layouts.
	assert((first_warp && splat_start == vk::ImageLayout::eUndefined &&
	        target_start == vk::ImageLayout::eUndefined) ||
	       (!first_warp && splat_start == vk::ImageLayout::eGeneral &&
	        target_start == vk::ImageLayout::eShaderReadOnlyOptimal));
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
	        *reinterpret_cast<const vk::ClearColorValue *>(splat_empty_bytes().bytes),
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
	// WV-R11: clear input must be the TRANSFER_DST_OPTIMAL the pre
	// barrier produced; output GENERAL is what the splat pass reads.
	assert(pre_barriers[0].newLayout == vk::ImageLayout::eTransferDstOptimal);
	assert(clear_barriers[0].oldLayout == vk::ImageLayout::eTransferDstOptimal &&
	       clear_barriers[0].newLayout == vk::ImageLayout::eGeneral);
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eTransfer,
	        vk::PipelineStageFlagBits::eComputeShader,
	        {},
	        {},
	        {},
	        clear_barriers);

	// Splat/pull/fill sets were written once at construction; only the UBO
	// contents above changed.
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *splat_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *splat_layout, 0, splat_sets[view], {});
	// WV-B5: far-plane clamp derived from this view's near/far (not
	// hardcoded 0.995 ~= 1.4 m at near=0.007). Mesh parity: sky at exactly
	// 1.0 must not reject-and-hole on backward motion, so clamp to
	// ~half-far depth instead (still background, representably < 1.0).
	reproject_splat_push splat_push_data{splat_far_clamp_depth(near_z, far_z)};
	cmd.pushConstants<reproject_splat_push>(*splat_layout, vk::ShaderStageFlagBits::eCompute, 0, splat_push_data);
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
	// Mip-chain hole fill: blit the premultiplied pull output down the
	// chain, then patch black pixels with the local average. Fixed
	// function (no shader ALU), constant cost per hole pixel: blurred
	// fill, never stretched.
	// Without blit support the fill pass cannot run: leave the pull
	// output as-is (holes stay black, pre-fill behavior).
	if (blit_supported)
	{
		// Pull writes must be visible before the blits read level 0.
		std::array fill_sync_barriers{
		        vk::ImageMemoryBarrier{
		                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
		                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
		                .image = target[view],
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .baseMipLevel = 0,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        },
		};
		// WV-R11: fill_sync input must be the GENERAL the pull pass wrote
		// (via clear GENERAL).
		assert(fill_sync_barriers[0].oldLayout == vk::ImageLayout::eGeneral);
		cmd.pipelineBarrier(
		        vk::PipelineStageFlagBits::eComputeShader,
		        vk::PipelineStageFlagBits::eTransfer,
		        {},
		        {},
		        {},
		        fill_sync_barriers);
	
		uint32_t mw = extent.width;
		uint32_t mh = extent.height;
		vk::ImageLayout mips_old = first_warp ? vk::ImageLayout::eUndefined
		        : vk::ImageLayout::eShaderReadOnlyOptimal;
		vk::AccessFlags mips_src_access =
		        first_warp ? vk::AccessFlags{} : vk::AccessFlagBits::eShaderRead;
		vk::PipelineStageFlags mips_src_stage = first_warp
		        ? vk::PipelineStageFlagBits::eTopOfPipe
		        : vk::PipelineStageFlagBits::eComputeShader;
		// WV-R11: mips keep SHADER_READ_ONLY across warps and restart
		// from UNDEFINED on the first warp (mips_old/first_warp).
		assert((first_warp && mips_old == vk::ImageLayout::eUndefined) ||
		       (!first_warp && mips_old == vk::ImageLayout::eShaderReadOnlyOptimal));
		for (uint32_t i = 1; i < mip_levels; i++)
		{
			std::array blit_pre_barriers{
			        vk::ImageMemoryBarrier{
			                .srcAccessMask = mips_src_access,
			                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
			                .oldLayout = mips_old,
			                .newLayout = vk::ImageLayout::eTransferDstOptimal,
			                .image = target[view],
			                .subresourceRange = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseMipLevel = i,
			                        .levelCount = 1,
			                        .layerCount = 1,
			                },
			        },
			};
			cmd.pipelineBarrier(
			        mips_src_stage,
			        vk::PipelineStageFlagBits::eTransfer,
			        {},
			        {},
			        {},
			        blit_pre_barriers);
			vk::ImageBlit blit{};
			blit.srcSubresource = {
			        vk::ImageAspectFlagBits::eColor,
			        i - 1,
			        0,
			        1,
			};
			blit.srcOffsets[1] = vk::Offset3D{int32_t(mw), int32_t(mh), 1};
			mw = std::max(1u, mw / 2);
			mh = std::max(1u, mh / 2);
			blit.dstSubresource = {
			        vk::ImageAspectFlagBits::eColor,
			        i,
			        0,
			        1,
			};
			blit.dstOffsets[1] = vk::Offset3D{int32_t(mw), int32_t(mh), 1};
			// TEMPORARY bisect: barriers run, blit itself skipped.
			if (!kSkipBlitImage)
				cmd.blitImage(
				        target[view], vk::ImageLayout::eTransferSrcOptimal, target[view],
				        vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
			std::array blit_post_barriers{
			        vk::ImageMemoryBarrier{
			                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
			                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
			                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
			                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
			                .image = target[view],
			                .subresourceRange = {
			                        .aspectMask = vk::ImageAspectFlagBits::eColor,
			                        .baseMipLevel = i,
			                        .levelCount = 1,
			                        .layerCount = 1,
			                },
			        },
			};
			cmd.pipelineBarrier(
			        vk::PipelineStageFlagBits::eTransfer,
			        vk::PipelineStageFlagBits::eTransfer,
			        {},
			        {},
			        {},
			        blit_post_barriers);
		}
		// Levels 1..N become shader-readable for the fill tap; level 0
		// goes back to GENERAL for fill writes.
		// WV-R10: src mask is the WRITE|READ superset on purpose. Levels
		// 1..N-1 were last used as blit *sources* (TRANSFER_READ in
		// TRANSFER_SRC_OPTIMAL, see the blit_post barriers above); only the
		// top level was write-only. A pure WRITE mask relies on the
		// TRANSFER->COMPUTE edge plus luck; the superset cannot remove
		// needed sync. NOTE: GPU-hang-bisect territory (fill/blit chain)
		// — any bisect result must be re-validated on the failing driver
		// before trusting it (see WV-B7).
		std::array fill_read_barriers{
		        vk::ImageMemoryBarrier{
		                .srcAccessMask = vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eTransferRead,
		                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
		                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = target[view],
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .baseMipLevel = 1,
		                        .levelCount = mip_levels - 1,
		                        .layerCount = 1,
		                },
		        },
		        vk::ImageMemoryBarrier{
		                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
		                .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
		                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
		                .newLayout = vk::ImageLayout::eGeneral,
		                .image = target[view],
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .baseMipLevel = 0,
		                        .levelCount = 1,
		                        .layerCount = 1,
		                },
		        },
		};
		// WV-R11: level-0 vs fill-write window. Levels 1..N must be the
		// TRANSFER_SRC_OPTIMAL the fill_sync + blit_post barriers left
		// (mip_levels >= 2 floored at construction, so levels 1..N exist);
		// level 0 must be TRANSFER_SRC_OPTIMAL heading back to GENERAL for
		// fill writes. Debug-only; release no-op.
		assert(mip_levels >= 2);
		assert(fill_sync_barriers[0].newLayout == vk::ImageLayout::eTransferSrcOptimal);
		assert(fill_read_barriers[0].oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
		       fill_read_barriers[0].newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
		assert(fill_read_barriers[1].oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
		       fill_read_barriers[1].newLayout == vk::ImageLayout::eGeneral);
		cmd.pipelineBarrier(
		        vk::PipelineStageFlagBits::eTransfer,
		        vk::PipelineStageFlagBits::eComputeShader,
		        {},
		        {},
		        {},
		        fill_read_barriers);
	
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *fill_pipeline);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *fill_layout, 0, fill_sets[view], {});
		// View-relative LOD cap: the fill view covers levels 1..N.
		int32_t max_view_lod = int32_t(mip_levels) - 2;
		cmd.pushConstants<int32_t>(*fill_layout, vk::ShaderStageFlagBits::eCompute, 0, max_view_lod);
		// TEMPORARY bisect: blits run, fill dispatch skipped. WV-B1: fill
		// requires blits (it samples the blit-built mip chain), so the
		// dispatch also requires !kSkipBlitImage; the static_assert above
		// makes fill-without-blits unrepresentable.
		if (!kSkipFillDispatch && !kSkipBlitImage)
			cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
	}
	

	// Target level 0 ends SHADER_READ_ONLY_OPTIMAL, ready to feed the
	// compute sampling in foveation (mip levels 1..N stay shader-read
	// from the fill pass). Splat scratch stays GENERAL for the next
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
	// WV-R11: level 0 is GENERAL here on both paths (fill wrote it,
	// or pull wrote it when blits are unsupported); it ends
	// SHADER_READ_ONLY_OPTIMAL feeding foveation. Debug-only.
	assert(out_barriers[0].oldLayout == vk::ImageLayout::eGeneral &&
	       out_barriers[0].newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
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
