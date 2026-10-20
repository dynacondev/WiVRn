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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#include "compositor.h"

// Monado includes
#include "driver/xrt_cast.h"
#include "main/comp_frame.h"
#include "util/comp_layer_accum.h"
#include "util/comp_render_helpers.h"
#include "util/comp_vulkan.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_time.h"
#include "vk/vk_helpers.h"

#include "driver/configuration.h"
#include "driver/wivrn_session.h"
#include "encoder/video_encoder.h"
#include "inplace_vector.hpp"
#include "utils/method.h"
#include "utils/wivrn_trace.h"

#include "xrt/xrt_config_build.h" // IWYU pragma: keep

#include <algorithm>
#include <chrono>
#ifdef XRT_FEATURE_RENDERDOC
#include "renderdoc_app.h"

static auto renderdoc()
{
	auto x = []() {
		RENDERDOC_API_1_5_0 * rdoc_api = nullptr;
		const char * env = std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
		if (not env or env != std::string_view("1"))
			return rdoc_api;
		void * mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
			XRT_MAYBE_UNUSED int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_5_0, (void **)&rdoc_api);
			assert(ret == 1);
		}
		return rdoc_api;
	};
	static auto res = x();
	return res;
}
#endif

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)

namespace details
{
template <auto Method, typename Result, typename... Args>
struct method_trait<Method, Result (wivrn::compositor::*)(Args...)>
{
	static Result magic(xrt_compositor * arg, Args... args)
	{
		return std::invoke(
		        Method,
		        static_cast<wivrn::compositor *>(reinterpret_cast<struct comp_base *>(arg)),
		        args...);
	}
};

} // namespace details

namespace
{
const comp_swapchain_image & get_layer_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index))->images[image_index];
}

const struct comp_swapchain_image &
get_layer_depth_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_depth_swapchain(&layer, swapchain_index))->images[image_index];
}

// Synthetic frames generated per real commit, covering sources down to
// ~10 fps at 90 Hz. Beyond this the oldest ticks repeat.
constexpr int max_synth_per_commit = 8;

std::array<vk::Format, 3> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

std::array<wivrn::compositor::image, 4> make_images(wivrn::vk_bundle & vk, vk::CommandPool command_pool, std::span<wivrn::encoder_settings> encoders)
{
	auto formats = image_formats(encoders[0].bit_depth);

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {
	                        .width = encoders[0].width,
	                        .height = encoders[0].height,
	                        .depth = 1,
	                },
	                .mipLevels = 1,
	                .arrayLayers = 3, // left, right then alpha
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
#if WIVRN_USE_VULKAN_ENCODE
	if (
	        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	        std::ranges::contains(
	                encoders,
	                wivrn::encoder_vulkan,
	                &wivrn::encoder_settings::encoder_name))
	{
		image_info.get().flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		image_info.get().usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	auto make_image = [&](int i) {
		vk::ImageViewUsageCreateInfo usage{
		        .usage = vk::ImageUsageFlagBits::eStorage,
		};
		image_allocation image{
		        vk.device,
		        image_info.get(),
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("compositor YCbCr image {}", i),
		};
		vk::Image vk_image{image};
		return wivrn::compositor::image{
		        .image{std::move(image)},
		        .view_y{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[0],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        },
		        .view_cbcr{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[1],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        }};
	};

	return {make_image(0), make_image(1), make_image(2), make_image(3)};
}

vk::raii::Semaphore make_semaphore(wivrn::vk_bundle & vk)
{
	vk::raii::Semaphore res{
	        vk.device,
	        vk::StructureChain{
	                vk::SemaphoreCreateInfo{},
	                vk::SemaphoreTypeCreateInfo{
	                        .semaphoreType = vk::SemaphoreType::eTimeline,
	                },
	        }
	                .get(),
	};
	vk.name(res, "compositor semaphore");
	return res;
}

vk::Extent3D render_extent(const wivrn::from_headset::headset_info_packet & info)
{
	return {
	        .width = info.render_eye_width,
	        .height = info.render_eye_height,
	        .depth = 1,
	};
}

} // namespace

namespace wivrn
{

void compositor::timings::add(float us)
{
	int index = this->index;
	this->index = (this->index + 1) % values.size();
	values[index] = us;
}

xrt_result_t compositor::predict_frame(int64_t * out_frame_id,
                                       int64_t * out_wake_time_ns,
                                       int64_t * out_predicted_gpu_time_ns,
                                       int64_t * out_predicted_display_time_ns,
                                       int64_t * out_predicted_display_period_ns)
{
	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t predicted_display_time_ns = 0;
	pacer.predict(
	        frame_id,
	        wake_up_time_ns,
	        desired_present_time_ns,
	        present_slop_ns,
	        predicted_display_time_ns);

	frame.waited.id = frame_id;
	frame.waited.desired_present_time_ns = desired_present_time_ns;
	frame.waited.present_slop_ns = present_slop_ns;
	frame.waited.predicted_display_time_ns = predicted_display_time_ns;

	*out_frame_id = frame_id;
	*out_wake_time_ns = wake_up_time_ns;
	*out_predicted_gpu_time_ns = desired_present_time_ns; // Not quite right but close enough.
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_predicted_display_period_ns = pacer.get_frame_duration();
	return XRT_SUCCESS;
}

xrt_result_t compositor::mark_frame(int64_t frame_id,
                                    xrt_compositor_frame_point point,
                                    int64_t when_ns)
{
	switch (point)
	{
		case XRT_COMPOSITOR_FRAME_POINT_WOKE:
			trace::instant_feedback("wake_up", when_ns, frame_id);
			return XRT_SUCCESS;
		default:
			assert(false);
	}
	return XRT_ERROR_VULKAN;
}

xrt_result_t compositor::layer_commit(xrt_graphics_sync_handle_t sync_handle)
{
	u_graphics_sync_unref(&sync_handle);

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&frame.rendering, &frame.waited);

	U_LOG_IFL_D(log_level, "frame %ld commit %d layers", frame.rendering.id, layer_accum.layer_count);

	if (not session.connected() or not session.get_offset())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

	// Commit-rate + depth-presence heartbeat (logging only: answers "is
	// the app submitting?" and "is depth arriving?" in one line so a
	// quiet warp section is distinguishable from a starved pipeline).
	// Only the log line is throttled; the commit path is untouched.
	static uint64_t last_rate_log_ns = 0;
	static uint64_t commits_since_log = 0;
	static uint64_t depth_commits_since_log = 0;
	{
		uint64_t now_ns = os_monotonic_get_ns();
		if (last_rate_log_ns != 0 and now_ns - last_rate_log_ns > 5'000'000'000ull)
		{
			double dt = (now_ns - last_rate_log_ns) / 1e9;
			U_LOG_I("compositor: %.1f commits/s (%llu with depth)", commits_since_log / dt, (unsigned long long)depth_commits_since_log);
			commits_since_log = 0;
			depth_commits_since_log = 0;
			last_rate_log_ns = now_ns;
		}
		if (last_rate_log_ns == 0)
			last_rate_log_ns = now_ns;
		++commits_since_log;
	}

	// Missed-tick detection for depth reprojection: each display
	// period without a real commit past the first means the application
	// missed a deadline (e.g. 45 fps app at 90 Hz needs 1 synthetic per
	// commit, 30 fps needs 2). Refined below (depth + resources).
	int synth_wanted = 0;
	const int64_t frame_period_ns = pacer.get_frame_duration();
	if (last_real_predicted_ns)
	{
		int64_t gap = frame.rendering.predicted_display_time_ns - *last_real_predicted_ns;
		synth_wanted = int((gap + frame_period_ns / 2) / frame_period_ns) - 1;
		synth_wanted = std::clamp(synth_wanted, 0, max_synth_per_commit);
	}
	last_real_predicted_ns = frame.rendering.predicted_display_time_ns;

	int i = acquire_image();
	if (i < 0)
	{
		// No free present slot: the encoder/GPU is backed up (or the pool
		// is undersized for the current load). The frame is dropped
		// silently by design; log throttled so a sustained stall is
		// visible instead of a silent freeze.
		static uint64_t last_acquire_fail_ns = 0;
		uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns - last_acquire_fail_ns > 5'000'000'000ull)
		{
			last_acquire_fail_ns = now_ns;
			U_LOG_W("compositor: no free present image, dropping real frame (encoder/GPU backed up?)");
		}
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->StartFrameCapture(NULL, NULL);
#endif

	auto & view_info = images[i].view_info;
	images[i].frame_index = frame.rendering.id;
	view_info = {
	        .display_time = session.get_offset().to_headset(frame.rendering.predicted_display_time_ns),
	        .alpha = layer_accum.data.env_blend_mode == XRT_BLEND_MODE_ALPHA_BLEND,
	};

	trace::instant_feedback("begin", frame.rendering.id, os_monotonic_get_ns());

	cmd_pool.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	cmd.resetQueryPool(*query_pool, 0, 4);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	bool flip_y = false;
	std::array<vk::ImageView, 2> src;
	std::array<xrt_rect, 2> src_rect;
	std::array<xrt_fov, 2> src_fov;

	// Depth reprojection inputs, valid only when has_depth_src is set
	// below (single fullscreen projection layer with depth).
	std::array<vk::ImageView, 2> src_depth;
	std::array<std::array<float, 4>, 2> src_color_nrect{};
	std::array<std::array<float, 4>, 2> src_depth_nrect{};
	std::array<float, 2> src_near_z{1, 1};
	std::array<float, 2> src_far_z{1000, 1000};
	std::array<xrt_pose, 2> src_pose{};
	bool has_depth_src = false;

	// Synthetic frames for missed display ticks, filled by the warp
	// section below (at most max_synth_per_commit per commit).
	struct synth_frame
	{
		int image = -1;
		int64_t frame_id = -1;
		int64_t desired_present_ns = 0;
		int64_t predicted_ns = 0;
		std::array<xrt_pose, 2> poses{};
		std::array<int8_t, 3> enc_slots{-1, -1, -1};
	};
	std::array<synth_frame, max_synth_per_commit> synths{};
	int synth_done = 0;

	beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 3> image_barriers;

	// Check if we can pass a layer directly to foveation
	if (layer_accum.layer_count == 1 and
	    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
	     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
	{
		const auto & layer = layer_accum.layers[0];
		for (int view = 0; view < 2; ++view)
		{
			const auto & data = (layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v)[view];
			src[view] = get_image_view(
			        &get_layer_image(layer, view, data.sub.image_index),
			        layer.data.flags,
			        data.sub.array_index);
			src_rect[view] = data.sub.rect;
			src_fov[view] = data.fov;
			flip_y = layer.data.flip_y;
			view_info.pose[view] = xrt_cast(data.pose);
			view_info.fov[view] = xrt_cast(data.fov);
			src_pose[view] = data.pose;
			const auto & cnr = data.sub.norm_rect;
			src_color_nrect[view] = {cnr.x, cnr.y, cnr.w, cnr.h};
			if (layer.data.type == XRT_LAYER_PROJECTION_DEPTH)
			{
				const auto & dvd = layer.data.depth.d[view];
				src_depth[view] = get_image_view(
				        &get_layer_depth_image(layer, view, dvd.sub.image_index),
				        layer.data.flags,
				        dvd.sub.array_index);
				const auto & dnr = dvd.sub.norm_rect;
				src_depth_nrect[view] = {dnr.x, dnr.y, dnr.w, dnr.h};
				src_near_z[view] = dvd.near_z;
				src_far_z[view] = dvd.far_z;
			}
		}
		if (layer.data.type == XRT_LAYER_PROJECTION_DEPTH)
		{
			has_depth_src = true;
			++depth_commits_since_log;
		}
	}
	else
	{
		// no fast-path, squash layers
		std::array<xrt_pose, 2> poses;
		const auto extent = images[0].image.info().extent;
		std::tie(poses, src_fov, src_rect) = squasher.do_layers(
		        vk.device,
		        cmd,
		        session.get_hmd(),
		        pacer.get_frame_duration(),
		        frame.rendering,
		        layer_accum,
		        xrt_rect{.extent{.w = int(extent.width), .h = int(extent.height)}});

		src = squasher.get_views();

		for (int view = 0; view < 2; ++view)
		{
			view_info.pose[view] = xrt_cast(poses[view]);
			view_info.fov[view] = xrt_cast(src_fov[view]);
		}

		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = squasher.get_image(),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 2,
		                },
		        });
	}

	image_barriers.push_back(
	        vk::ImageMemoryBarrier2{
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = images[i].image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = images[i].image.info().arrayLayers,
	                },
	        });

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	image_barriers.clear();

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

	// Hold the current frame: copy color + depth into the
	// compositor-owned retained sources so every synthetic below warps
	// from held data, valid until the next real frame refreshes it.
	if (has_depth_src and depth_reprojection_enabled)
		reprojector.refresh_retained(vk.device, cmd, src, src_depth, src_color_nrect, src_depth_nrect, flip_y);

	// Warp circuit breaker: a degenerate splat (e.g. half the frame
	// unmapped after a huge inter-frame jump at low app fps) turns
	// every hole pixel into a full-radius expanding-ring search —
	// hundreds of millions of taps per synthetic that wedge the GPU
	// until the client gives up. If the last commit's hole fraction was
	// catastrophic, skip the warp for a cooldown and emit repeats
	// instead. Time-based, so recovery is automatic.
	uint64_t warp_now_ns = os_monotonic_get_ns();
	static uint64_t warp_cooldown_until_ns = 0;
	const uint64_t warp_cooldown_ns = 5'000'000'000ull;
	bool warp_cooldown = warp_now_ns < warp_cooldown_until_ns;

	// Depth reprojection: one synthetic frame per missed display tick
	// (synth_wanted, from the predicted-time gap), each warped from the
	// retained sources to its own upcoming tick so the encoder still
	// emits at headset cadence. True frames keep the existing rotation
	// timewarp in the squasher/fast-path; synthetics replace it with a
	// full 6DoF compute warp.
	if (synth_wanted and has_depth_src and depth_reprojection_enabled and not warp_cooldown)
	{
		try
		{
			for (int k = 0; k < synth_wanted; ++k)
			{
				synth_frame & s = synths[k];
				int64_t wake_ns, slop_ns;
				pacer.predict(s.frame_id, wake_ns, s.desired_present_ns, slop_ns, s.predicted_ns);

			s.image = acquire_image();
			if (s.image < 0)
			{
				// Same story as the real-frame path above: slots are full
				// because the encoder/GPU can't drain at this load. Skip
				// the remaining synthetics (degrade to repeat) and say so,
				// throttled — this is the signal that distinguishes "no
				// missed ticks" from "too backed up to synthesize".
				static uint64_t last_synth_acquire_fail_ns = 0;
				if (warp_now_ns - last_synth_acquire_fail_ns > 5'000'000'000ull)
				{
					last_synth_acquire_fail_ns = warp_now_ns;
					U_LOG_W("Depth warp: no free present image, skipping %d synthetics (repeat instead)",
					        synth_wanted - synth_done);
				}
				break;
			}

				xrt_space_relation head{};
				std::array<xrt_fov, 2> fovs{};
				session.get_hmd().get_view_poses(
				        nullptr,
				        s.predicted_ns,
				        XRT_VIEW_TYPE_STEREO,
				        2,
				        &head,
				        fovs.data(),
				        s.poses.data());

				// The tracker may be unable to serve a synthetic tick
				// (too far ahead, momentary tracking loss): it reports
				// that via the head relation flags while the eye poses
				// fall back to identity+offsets. Warping to those
				// produces a fully degenerate splat (trips the breaker
				// and blinds the warp for 5 s), so skip synthetics on
				// invalid head poses instead, degrading to repeat.
				constexpr auto need_pose = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
				if ((head.relation_flags & need_pose) != need_pose)
				{
					static uint64_t last_invalid_pose_log_ns = 0;
					if (warp_now_ns - last_invalid_pose_log_ns > 5'000'000'000ull)
					{
						last_invalid_pose_log_ns = warp_now_ns;
						U_LOG_I("Depth warp: skipping %d synthetics (head pose invalid for tick, repeat instead)",
						        synth_wanted - synth_done);
					}
					images[s.image].busy = false;
					s.image = -1;
					break;
				}

				bool ok = true;
				for (int view = 0; view < 2; ++view)
				{
					ok &= reprojector.reproject(
					        vk.device,
					        cmd,
					        view,
					        src_pose[view],
					        src_fov[view],
					        src_near_z[view],
					        src_far_z[view],
					        s.poses[view],
					        src_fov[view]);
				}
				if (not ok)
				{
					images[s.image].busy = false;
					s.image = -1;
					break;
				}
				++synth_done;
			}
		}
		catch (std::exception & e)
		{
			// A Vulkan/alloc throw mid-warp must never wedge the
			// compositor thread (that wedges streaming until a server
			// restart). Release the in-flight image, keep any completed
			// synthetics, cool down, and repeat this commit.
			U_LOG_E("Depth warp threw (%s); repeating frames for 5 s", e.what());
			for (int k = synth_done; k < synth_wanted; ++k)
			{
				if (synths[k].image >= 0)
				{
					images[synths[k].image].busy = false;
					synths[k].image = -1;
				}
			}
			warp_cooldown_until_ns = warp_now_ns + warp_cooldown_ns;
		}
		if (synth_done)
		{
			// Proof-of-life for the warp + tuning signal for hole_radius:
			// unfilled pixels across this commit's synthetics. Throttled:
			// at most one line per 5 s so a struggling scene can't spam.
			uint32_t holes = reprojector.drain_hole_count();
			static uint64_t last_warp_log_ns = 0;
			if (warp_now_ns - last_warp_log_ns > 5'000'000'000ull)
			{
				last_warp_log_ns = warp_now_ns;
				U_LOG_I("Depth warp: %d synthetics from retained depth (%u unfilled hole pixels)",
				        synth_done, holes);
			}
			// Catastrophic hole fraction (> half of all target pixels):
			// the splat is degenerate, not disoccluded. Trip the breaker
			// rather than melting the GPU on full-radius searches.
			const auto warp_extent = images[0].image.info().extent;
			uint64_t warp_pixels =
			        2ull * uint64_t(warp_extent.width) * uint64_t(warp_extent.height);
			if (warp_pixels > 0 and uint64_t(holes) > warp_pixels / 2)
			{
				warp_cooldown_until_ns = warp_now_ns + warp_cooldown_ns;
				U_LOG_W("Depth warp: %u/%llu pixels unfilled, pausing warp for 5 s (repeat instead)",
				        holes, (unsigned long long)warp_pixels);
				// Input dump for the degenerate case: a clean commit
				// followed by a catastrophic one within milliseconds
				// points at transient inputs (pose jump, degenerate
				// rect, wrong near/far), not scene motion. All CPU-side,
				// no GPU readback.
				for (int view = 0; view < 2; ++view)
				{
					const auto & sp = src_pose[view];
					U_LOG_W("Depth warp input v%d: src pos=(%.3f,%.3f,%.3f) quat=(%.3f,%.3f,%.3f,%.3f) fov=(%.3f,%.3f,%.3f,%.3f) near=%.4f far=%.1f color_rect=(%.3f,%.3f,%.3f,%.3f) depth_rect=(%.3f,%.3f,%.3f,%.3f)",
					        view,
					        sp.position.x, sp.position.y, sp.position.z,
					        sp.orientation.x, sp.orientation.y, sp.orientation.z, sp.orientation.w,
					        src_fov[view].angle_left, src_fov[view].angle_right, src_fov[view].angle_up, src_fov[view].angle_down,
					        src_near_z[view], src_far_z[view],
					        src_color_nrect[view][0], src_color_nrect[view][1], src_color_nrect[view][2], src_color_nrect[view][3],
					        src_depth_nrect[view][0], src_depth_nrect[view][1], src_depth_nrect[view][2], src_depth_nrect[view][3]);
				}
				for (int k = 0; k < synth_done; ++k)
				{
					const synth_frame & s = synths[k];
					for (int view = 0; view < 2; ++view)
					{
						const auto & tp = s.poses[view];
						U_LOG_W("Depth warp target k%d v%d: pos=(%.3f,%.3f,%.3f) quat=(%.3f,%.3f,%.3f,%.3f) predicted=%lldms desired=%lldms",
						        k, view,
						        tp.position.x, tp.position.y, tp.position.z,
						        tp.orientation.x, tp.orientation.y, tp.orientation.z, tp.orientation.w,
						        (long long)(s.predicted_ns / 1'000'000), (long long)(s.desired_present_ns / 1'000'000));
					}
				}
			}
		}
	}
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

	if (session.get_info().eye_gaze)
	{
		auto now = os_monotonic_get_ns();
		session.add_tracking_request(device_id::EYE_GAZE, frame.rendering.desired_present_time_ns, now, now);
	}
	view_info.foveation = foveation.foveate(
	        vk.device,
	        cmd,
	        images[i].view_y,
	        images[i].view_cbcr,
	        flip_y,
	        src,
	        src_rect,
	        src_fov,
	        view_info.alpha);

	// Synthetic frames sample the warped targets (full-eye, always
	// GL-ordered by construction, hence flip_y = true) through the same
	// foveation. All synthetics share rect and fov; only outputs differ.
	if (synth_done)
	{
		const auto synth_extent = images[0].image.info().extent;
		std::array<xrt_rect, 2> synth_rect{
		        xrt_rect{
		                .offset = {.w = 0, .h = 0},
		                .extent = {.w = int(synth_extent.width), .h = int(synth_extent.height)},
		        },
		        xrt_rect{
		                .offset = {.w = 0, .h = 0},
		                .extent = {.w = int(synth_extent.width), .h = int(synth_extent.height)},
		        },
		};
		for (int k = 0; k < synth_done; ++k)
		{
			synth_frame & s = synths[k];
			auto synth_foveation = foveation.foveate(
			        vk.device,
			        cmd,
			        images[s.image].view_y,
			        images[s.image].view_cbcr,
			        true, // warped targets are always GL-ordered
			        reprojector.get_views(),
			        synth_rect,
			        src_fov,
			        view_info.alpha);

			images[s.image].frame_index = uint64_t(s.frame_id);
			images[s.image].view_info = {
			        .display_time = session.get_offset().to_headset(s.predicted_ns),
			        .alpha = view_info.alpha,
			};
			for (int view = 0; view < 2; ++view)
			{
				images[s.image].view_info.pose[view] = xrt_cast(s.poses[view]);
				images[s.image].view_info.fov[view] = xrt_cast(src_fov[view]);
				images[s.image].view_info.foveation[view] = synth_foveation[view];
			}
		}
	}

	auto append_encoder_barriers = [&](int image_idx, const to_headset::video_stream_data_shard::view_info_t & info) {
		for (auto & encoder: encoders)
		{
			if (encoder->stream_idx == 2 and not info.alpha)
				continue;
			else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
			{
				image_barriers.push_back(
				        vk::ImageMemoryBarrier2{
				                .srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
				                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
				                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
				                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
				                // For a queue-family ownership transfer in EXCLUSIVE
				                // sharing mode, the release barrier's old/new layout
				                // must match the encoder-side acquire. newLayout is
				                // the encoder's target_layout; per the VkImageMemoryBarrier2
				                // spec the layout transition is executed exactly once
				                // between the queues, so this single QFOT barrier covers
				                // both the queue-family transfer and the layout
				                // transition the encoder needs.
				                .oldLayout = vk::ImageLayout::eGeneral,
				                .newLayout = encoder->target_layout,
				                .srcQueueFamilyIndex = vk.queue.family_index,
				                .dstQueueFamilyIndex = encoder->target_queue,
				                .image = images[image_idx].image,
				                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
				                                     .baseMipLevel = 0,
				                                     .levelCount = 1,
				                                     .baseArrayLayer = encoder->stream_idx,
				                                     .layerCount = 1},
				        });
			}
		}
	};

	append_encoder_barriers(i, view_info);
	for (int k = 0; k < synth_done; ++k)
		append_encoder_barriers(synths[k].image, images[synths[k].image].view_info);

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 3);

	cmd.end();

	const vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = ++sem_value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};

	{
		vk::CommandBufferSubmitInfo cmd_info{
		        .commandBuffer = cmd,
		};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmd_info,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &sem_info,
		});
	}

	pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
	auto info = pacer.present_to_info(frame.rendering.desired_present_time_ns);

	// Record each encoder's slot on the queued frame; encode() consumes
	// exactly the recorded slot, so present/encode stay paired across
	// exceptions, drops and skips (no shared counters to diverge).
	std::array<int8_t, 3> real_slots{-1, -1, -1};
	try
	{
		for (size_t ei = 0; ei < encoders.size(); ++ei)
		{
			auto & encoder = encoders[ei];
			if (encoder->stream_idx == 2 and not view_info.alpha)
				continue;
			real_slots[ei] = int8_t(encoder->present_image(
			        images[i].image,
			        sem_info,
			        info.frame_id));
		}

		// Synthetic frames are encoded right behind the real frame, in
		// display order (real tick first, then missed ticks). Backpressure
		// is natural: if the encoder cannot sustain headset cadence the
		// present slots stay busy, acquire_image fails and later synthetics
		// are skipped, degrading to repeat. Synth presents are bounded
		// (wait=false): a pressured pool skips synthetics instead of ever
		// deadlocking the commit when the burst exceeds the slot count.
		for (int k = 0; k < synth_done; ++k)
		{
			synth_frame & s = synths[k];
			auto synth_info = pacer.present_to_info(s.desired_present_ns);
			bool skipped = false;
			for (size_t ei = 0; ei < encoders.size(); ++ei)
			{
				auto & encoder = encoders[ei];
				if (encoder->stream_idx == 2 and not images[s.image].view_info.alpha)
					continue;
				int synth_slot = encoder->present_image(
				        images[s.image].image,
				        sem_info,
				        synth_info.frame_id,
				        false);
				if (synth_slot < 0)
				{
					skipped = true;
					break;
				}
				s.enc_slots[ei] = int8_t(synth_slot);
			}
			if (skipped)
			{
				static uint64_t last_synth_skip_log_ns = 0;
				if (warp_now_ns - last_synth_skip_log_ns > 5'000'000'000ull)
				{
					last_synth_skip_log_ns = warp_now_ns;
					U_LOG_W("Depth warp: skipping %d synthetics at present (slots pressured, repeat instead)", synth_done - k);
				}
				for (size_t ei = 0; ei < encoders.size(); ++ei)
				{
					if (s.enc_slots[ei] >= 0)
					{
						encoders[ei]->release_slot(uint8_t(s.enc_slots[ei]));
						s.enc_slots[ei] = -1;
					}
				}
				images[s.image].busy = false;
				s.image = -1;
				for (int j = k + 1; j < synth_done; ++j)
				{
					if (synths[j].image >= 0)
						images[synths[j].image].busy = false;
					synths[j].image = -1;
				}
				synth_done = k;
				break;
			}
		}
	}
	catch (...)
	{
		// A present threw after earlier presents succeeded: those slots
		// and images would never be encoded (the frame is dropped), so
		// release them instead of leaking them busy, which wedges the
		// compositor forever with zero logs.
		U_LOG_E("compositor: present threw for frame %ld, releasing slots", (long)frame.rendering.id);
		for (size_t ei = 0; ei < encoders.size(); ++ei)
		{
			if (real_slots[ei] >= 0)
				encoders[ei]->release_slot(uint8_t(real_slots[ei]));
			for (int k = 0; k < synth_done; ++k)
			{
				if (synths[k].enc_slots[ei] >= 0)
					encoders[ei]->release_slot(uint8_t(synths[k].enc_slots[ei]));
			}
		}
		images[i].busy = false;
		for (int k = 0; k < synth_done; ++k)
		{
			if (synths[k].image >= 0)
				images[synths[k].image].busy = false;
		}
		throw;
	}

	queue_encode(i, real_slots);
	for (int k = 0; k < synth_done; ++k)
		queue_encode(synths[k].image, synths[k].enc_slots);

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_info.value,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		U_LOG_IFL_W(log_level, "compositor timeout");
	}
	else
	{
		auto [res, ts] = query_pool.getResults<uint64_t>(
		        0,
		        4,
		        4 * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[1] - ts[0]) * period / 1e3);
			warp_times.add((ts[2] - ts[1]) * period / 1e3);
			foveation_times.add((ts[3] - ts[2]) * period / 1e3);
		}
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&cscs);

	return XRT_SUCCESS;
}

xrt_result_t compositor::get_display_refresh_rate(float * hz)
{
	auto settings = session.get_settings();
	// there should not be rounding errors, hz must be one of the available refresh rates
	*hz = frame_rate * settings->fps_divider;
	assert(std::ranges::contains(session.get_info().available_refresh_rates, *hz));
	return XRT_SUCCESS;
}

xrt_result_t compositor::request_display_refresh_rate(float hz)
{
	requested_refresh_rate = hz;
	U_LOG_I("request refresh rate: %fHz", hz);
	if (hz > 0)
	{
		try
		{
			session.send_control(to_headset::refresh_rate_change{.hz = hz});
		}
		catch (std::exception & e)
		{
			U_LOG_W("refresh rate change failed: %s", e.what());
		}
	}
	return XRT_SUCCESS;
}

xrt_result_t compositor::get_view_config(
        xrt_compositor_native * self,
        xrt_view_type view_type,
        xrt_view_config * out_view_config)
{
	const auto & session = reinterpret_cast<compositor *>(self)->session;
	const auto extent = render_extent(session.get_info());
	switch (view_type)
	{
		case XRT_VIEW_TYPE_MONO:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
		case XRT_VIEW_TYPE_STEREO:
			*out_view_config = {
			        .view_type = XRT_VIEW_TYPE_STEREO,
			        .view_count = 2,
			        .views = {}};
			for (auto & view: std::span(out_view_config->views, out_view_config->view_count))
			{
				view = {
				        .recommended = {
				                .width_pixels = extent.width,
				                .height_pixels = extent.height,
				                .sample_count = 1,
				        },
				        .max = {
				                .width_pixels = extent.width * 2u,
				                .height_pixels = extent.height * 2u,
				                .sample_count = 1,
				        },
				};
			}
			return XRT_SUCCESS;
		case XRT_VIEW_TYPE_QUAD:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
	}
	return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
}

int compositor::acquire_image()
{
	for (auto [i, image]: std::ranges::enumerate_view(images))
	{
		if (not image.busy.exchange(true))
			return i;
	}
	return -1;
}

void compositor::queue_encode(int image, const std::array<int8_t, 3> & slots)
{
	{
		std::lock_guard lock(encode_mutex);
		encode_queue.push_back({image, slots});
		video_encoder::worker_queue_depth().store(encode_queue.size(), std::memory_order_relaxed);
	}
	encode_cv.notify_one();
}

void compositor::encoder_work(std::stop_token tok)
{
	while (true)
	{
		// Heartbeat every iteration, including idle waits: a stale
		// heartbeat in a stall log means this thread is parked or gone
		// (it never blocks unboundedly anywhere below).
		video_encoder::worker_heartbeat_ns().store(os_monotonic_get_ns(), std::memory_order_relaxed);
		queued_frame req{-1};
		{
			std::unique_lock lock(encode_mutex);
			// Bounded stop-aware wait: an idle worker still heartbeats
			// instead of sleeping forever; stop still exits promptly.
			encode_cv.wait_for(lock, tok, std::chrono::milliseconds(100), [&] { return not encode_queue.empty(); });
			if (tok.stop_requested())
				return;
			if (encode_queue.empty())
				continue;
			req = std::move(encode_queue.front());
			encode_queue.pop_front();
			video_encoder::worker_queue_depth().store(encode_queue.size(), std::memory_order_relaxed);
		}
		video_encoder::worker_last_image().store(req.image, std::memory_order_relaxed);

		assert(req.image >= 0 and req.image < images.size());
		auto & image = images[req.image];

		wivrn::trace::scope trace_iter(wivrn::trace::cpu_track::compositor, 0, image.frame_index, "encoder_work iter");

		try
		{
			for (size_t ei = 0; ei < encoders.size(); ++ei)
			{
				auto & encoder = encoders[ei];
				if (req.slots[ei] < 0)
					continue;
				if (encoder->stream_idx < 2 or image.view_info.alpha)
					encoder->encode(session, image.view_info, image.frame_index, uint8_t(req.slots[ei]));
			}
		}
		catch (std::exception & e)
		{
			U_LOG_W("encode error: %s", e.what());
		}
		image.busy = false;
	}
}

void compositor::send_video_stream_description()

{
	to_headset::video_stream_description desc{
	        .width = uint16_t(images[0].image.info().extent.width),
	        .height = uint16_t(images[0].image.info().extent.height),
	        .frame_rate = settings[0].fps,
	};
	get_display_refresh_rate(&desc.refresh_rate);
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.codec)>);
	std::ranges::transform(settings, desc.codec.begin(), &encoder_settings::codec);
	session.send_control(std::move(desc));
}

compositor::compositor(wivrn_session & session) :
        comp_base{
                .base = {
                        .base = {
                                .info{},
                                .begin_session = method_pointer<&compositor::begin_session>,
                                .end_session = method_pointer<&compositor::end_session>,
                                .predict_frame = method_pointer<&compositor::predict_frame>,
                                .mark_frame = method_pointer<&compositor::mark_frame>,
                                .begin_frame = method_pointer<&compositor::begin_frame>,
                                .discard_frame = method_pointer<&compositor::discard_frame>,
                                .layer_commit = method_pointer<&compositor::layer_commit>,
                                .get_display_refresh_rate = method_pointer<&compositor::get_display_refresh_rate>,
                                .request_display_refresh_rate = method_pointer<&compositor::request_display_refresh_rate>,
                                .destroy = method_pointer<&compositor::destroy>,
                        },
                },
        },
        log_level(debug_get_log_option_log()),
        session(session),
        cmd_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        query_pool(vk.device, vk::QueryPoolCreateInfo{
                                      .queryType = vk::QueryType::eTimestamp,
                                      .queryCount = 4,
                              }),
        settings(get_encoder_settings(vk, session)),
        images{make_images(vk, cmd_pool, settings)},
        cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1})[0])},
        sem{make_semaphore(vk)},
        frame_rate(settings[0].fps),
        pacer(U_TIME_1S_IN_NS / frame_rate),
        squasher(vk, render_extent(session.get_info())),
        foveation(vk, images[0].image.info().extent),
        reprojector(vk, vk::Extent2D{images[0].image.info().extent.width, images[0].image.info().extent.height}),
        depth_reprojection_enabled(configuration().reprojection.enabled)
{
	comp_base * c_base = this;
	// Ensure we can safely cast pointers
	assert(intptr_t(&base) == intptr_t(this));
	auto res = vk_init_from_given(
	        &c_base->vk,
	        (PFN_vkGetInstanceProcAddr)vk.instance.getProcAddr("vkGetInstanceProcAddr"),
	        *vk.instance,
	        *vk.physical_device,
	        *vk.device,
	        vk.queue.family_index,
	        0,
	        vk.has_device_ext(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME),
	        vk.has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
	        std::get<vk::PhysicalDeviceVulkan12Features>(vk.feat).timelineSemaphore,
	        true, // in Vulkan 1.2
	        vk.has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	        log_level);
	vk::detail::resultCheck(vk::Result(res), "vk_init_from_given");

	c_base->vk.version = vk_bundle::api_version;
	// vk_init_from_given can't enable calibrated timestamps; do it here.
#ifdef VK_EXT_calibrated_timestamps
	c_base->vk.has_EXT_calibrated_timestamps = vk.has_device_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

	// Share monado's vk_bundle so gpu_timestamp_pool reuses its calibration cache.
	wivrn::trace::set_calibration_source(&c_base->vk);

	// vk_init_from_given assumes a graphics queue was provided
	c_base->vk.graphics_queue = nullptr;

	// Monado submits to the main queue from IPC client threads under
	// main_queue->mutex: our own submissions must take the same lock.
	vk::detail::resultCheck(vk::Result(vk_init_mutex(&c_base->vk)), "vk_init_mutex");
	if (c_base->vk.main_queue)
		vk.queue.mutex.share(c_base->vk.main_queue->mutex.mutex);

	{
		comp_vulkan_formats formats{};
		comp_vulkan_formats_check(&c_base->vk, &formats);
		comp_vulkan_formats_copy_to_info(&formats, &base.base.info);
		comp_vulkan_formats_log(log_level, &formats);
	}

	comp_base_init(this);

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&cscs, &c_base->vk);
	if (xret != XRT_SUCCESS)
		throw std::runtime_error("comp_swapchain_shared_init failed");

	print_encoders(settings);
	for (auto [i, settings]: std::ranges::enumerate_view(settings))
		encoders[i] = video_encoder::create(vk, settings, i);
	send_video_stream_description();

	u_var_add_root(this, "Compositor", false);
	u_var_add_f32_timing(this, &squasher_times.var, "layers processing");
	u_var_add_f32_timing(this, &warp_times.var, "depth warp");
	u_var_add_f32_timing(this, &foveation_times.var, "foveation");

	// Start the thread after everything is initialized
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
}

compositor::~compositor()
{
	u_var_remove_root(this);
	encoder_thread.request_stop();
	encode_cv.notify_all();
	comp_base * c_base = this;
	comp_swapchain_shared_garbage_collect(&cscs);
	comp_swapchain_shared_destroy(&cscs, &c_base->vk);
	comp_base_fini(this);
}

xrt_system_compositor_info compositor::sys_info() const
{
	const auto & info = session.get_info();
	auto [prop, dev_id] = vk.physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	xrt_system_compositor_info res{
	        .view_type_count = 1,
	        .view_types = {
	                XRT_VIEW_TYPE_STEREO,
	        },
	        .max_layers = squasher.max_layers(prop.properties),
	        .supported_blend_modes = {
	                XRT_BLEND_MODE_OPAQUE,
	                XRT_BLEND_MODE_ALPHA_BLEND,
	        },
	        .supported_blend_mode_count = uint8_t(1 + info.passthrough),
	        .refresh_rate_count = std::min<uint32_t>(info.available_refresh_rates.size(), std::size(res.refresh_rates_hz)),
	        .supports_fov_mutable = true,
	};

	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
	std::ranges::copy(std::span(info.available_refresh_rates).subspan(0, res.refresh_rate_count), res.refresh_rates_hz);
	return res;
}

void compositor::set_bitrate(uint32_t bitrate)
{
	for (auto & encoder: encoders)
		encoder->set_bitrate(bitrate);
}

void compositor::set_framerate(float hz)
{
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	pacer.set_frame_duration(U_TIME_1S_IN_NS / hz);
	// Display ticks moved: forget the last real tick so the next gap
	// computation cannot fire a spurious synthetic.
	last_real_predicted_ns.reset();
	for (auto & encoder: encoders)
		encoder->set_framerate(hz);
}

void compositor::update_tracking(const from_headset::tracking & tracking)
{
	foveation.update_tracking(tracking);
}

void compositor::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	foveation.update_foveation_center_override(center);
}

void compositor::resume()
{
	for (auto & encoder: encoders)
		encoder->reset();
	last_real_predicted_ns.reset();
	send_video_stream_description();
}

void compositor::on_feedback(const from_headset::feedback & feedback, const clock_offset & o)
{
	uint8_t stream = feedback.stream_index;
	if (stream >= encoders.size())
		return;
	encoders[stream]->on_feedback(feedback);
	if (not o)
		return;
	pacer.on_feedback(feedback, o);
}

} // namespace wivrn
