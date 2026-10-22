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
#include "math/m_space.h"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
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

// Plausibility gate for depth warps (prototype parity): implied head
// velocity between source capture and target display. A tick this
// violent would splat mostly-holes (every hole pixel costs a
// full-radius expanding-ring search); a single repeat degrades far more
// gracefully. Pure CPU, no GPU readback.
static constexpr float kMaxWarpSpeedMps = 10.f;
static constexpr float kMaxWarpSpeedRps = 12.f;
static bool warp_motion_plausible(const std::array<xrt_pose, 2> & src,
                                  const std::array<xrt_pose, 2> & dst,
                                  double dt_s,
                                  float & worst_ms,
                                  float & worst_rs)
{
	worst_ms = 0.f;
	worst_rs = 0.f;
	if (!(dt_s > 0.))
		return false;
	for (int view = 0; view < 2; ++view)
	{
		const auto & a = src[view];
		const auto & b = dst[view];
		float dx = b.position.x - a.position.x;
		float dy = b.position.y - a.position.y;
		float dz = b.position.z - a.position.z;
		float dist = sqrtf(dx * dx + dy * dy + dz * dz);
		float dot = fabsf(a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
		                  a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w);
		dot = fminf(fmaxf(dot, 0.f), 1.f);
		float angle = 2.f * acosf(dot);
		worst_ms = fmaxf(worst_ms, float(dist / dt_s));
		worst_rs = fmaxf(worst_rs, float(angle / dt_s));
	}
	return worst_ms <= kMaxWarpSpeedMps && worst_rs <= kMaxWarpSpeedRps;
}

// Throttled skip note shared by both warp paths (same 5 s window:
// a violent motion episode trips both).
static void note_warp_skip_implausible(float worst_ms, float worst_rs, double dt_ms)
{
	static std::atomic<uint64_t> last_ns{0};
	uint64_t now_ns = uint64_t(os_monotonic_get_ns());
	if (now_ns - last_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
	{
		last_ns.store(now_ns, std::memory_order_relaxed);
		U_LOG_W("compositor: skipping warp (implausible head motion %.1f m/s %.1f rad/s dt=%.1fms, repeat instead)",
		        worst_ms, worst_rs, dt_ms);
	}
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

	// Staleness gate for the warp below: a commit is stale when the app
	// is running slower than the display, measured by inter-commit
	// spacing against the display period. layer_commit runs once per app
	// EndFrame, so timestamp comparison can never detect a repeat —
	// consecutive commits always carry fresh timestamps. A stale commit's
	// frame will still be on screen when the next display tick needs one,
	// so warping it to display time is pure win; at full rate this never
	// fires (zero cost, zero risk).
	const uint64_t commit_ns = os_monotonic_get_ns();
	const uint64_t display_period_ns = uint64_t(pacer.get_frame_duration());
	const uint64_t last_commit = last_commit_ns.load();
	// Hysteresis latch (not a pure threshold): arm when the spacing
	// exceeds 1.5 periods, release only when it drops back below one
	// period. A threshold alone flaps at exactly-half-rate apps,
	// alternating warped/plain frames every tick.
	if (last_commit != 0 && commit_ns - last_commit > display_period_ns + display_period_ns / 2)
		warp_armed_stale.store(true);
	else if (last_commit == 0 || commit_ns - last_commit < display_period_ns)
		warp_armed_stale.store(false);
	const bool stale_commit = warp_armed_stale.load();
	last_commit_ns.store(commit_ns);

	int i = acquire_image();

	// Pool exhaustion (real or synth): the encoder/GPU is backed up.
	// The tick degrades to drop/repeat by design; one throttled line so
	// a sustained stall is visible instead of a silent freeze.
	auto note_acquire_fail = [] {
		static uint64_t last_ns = 0;
		uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns - last_ns > 5'000'000'000ull)
		{
			last_ns = now_ns;
			U_LOG_W("compositor: no free present image (encoder/GPU backed up?)");
		}
	};
	if (i < 0)
	{
		note_acquire_fail();
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
	last_alpha_blend.store(view_info.alpha);

	trace::instant_feedback("begin", frame.rendering.id, os_monotonic_get_ns());

	// Serialized with the synth ticker (commit_mutex): record + submit
	// share the reprojector, foveation UBO / descriptor writes, cmd
	// buffer, sem_value and last_submit_value. Released before the
	// present calls (each encoder bounds its slot wait at 1 s) and
	// before the trailing GPU waits, so a wedged tick never parks the
	// other path — and the synth ticker does the same around its
	// record.
	std::unique_lock commit_lock{commit_mutex};

	// Hoisted for the catch + post-try tail: everything recorded or
	// submitted below runs inside the try guard (a Vulkan throw must
	// release acquired images/slots, never cross the C ABI), but the
	// catch and the queue tail live outside the try scope.
	struct commit_synth_frame
	{
		int image = -1;
		std::array<xrt_pose, 2> poses{};
		std::array<int8_t, 3> enc_slots{-1, -1, -1};
	};
	commit_synth_frame synth{};
	bool have_synth = false;
	std::array<xrt_fov, 2> synth_fov{};
	std::array<int8_t, 3> real_slots{-1, -1, -1};
	std::array<int8_t, 3> synth_slots{-1, -1, -1};
	uint64_t sem_value_now = 0;
	pacer::frame_info present_info{};
	bool warp_replaces_real = false;

	// Everything recorded or submitted below (resolve, warp, foveate)
	// runs inside one guard: a Vulkan throw (device loss, OOM)
	// must release the acquired images and recorded slots and return an
	// error — exceptions must not cross the C ABI out of layer_commit.
	try
	{
		// Shared upload state (UBOs, descriptor sets, resolve set) must
		// be idle before recording: a synth submit may still be on the
		// GPU (recording is serialized, execution is not). Bounded by one
		// period; on timeout drop the tick rather than recording against
		// in-flight work.
		if (!wait_for_last_submit(display_period_ns))
		{
			static std::atomic<uint64_t> last_pre_record_timeout_ns{0};
			uint64_t now_ns = uint64_t(os_monotonic_get_ns());
			if (now_ns - last_pre_record_timeout_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
			{
				last_pre_record_timeout_ns.store(now_ns, std::memory_order_relaxed);
				U_LOG_W("compositor: previous GPU work never completed, dropping frame");
			}
			images[i].busy = false;
			comp_frame_clear_locked(&frame.rendering);
			return XRT_SUCCESS;
		}
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

		// (synth/have_synth/synth_fov are hoisted above the try for the
		// catch + queue tail; reused here.)
		// App-frame freshness for refresh gating: a changed client timestamp
		// means this commit carries a new app frame. (Freshness can never
		// signal a repeat — see the staleness gate above.)
		bool app_fresh = true;
		int64_t app_layer_ts_ns = 0;

		beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> image_barriers;

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
			// Client xrEndFrame displayTime: identical across display-tick
			// replays of the same app frame, fresh on every new app frame.
			app_layer_ts_ns = layer.data.timestamp;
			int64_t last_ts = last_app_layer_ts.load();
			app_fresh = last_ts < 0 || last_ts != app_layer_ts_ns;
			last_app_layer_ts.store(app_layer_ts_ns);
			if (layer.data.type == XRT_LAYER_PROJECTION_DEPTH)
				has_depth_src = true;
		}
		else
		{
			// No fast-path, squash layers. Multi-layer frames carry no
			// depth source (warping a squashed composite has no valid
			// per-pixel depth), so has_depth_src stays false and the
			// synth ticker stands down until the next depth commit.
			// Fine for fullscreen X-Plane; a transient overlay pauses
			// synthesis while it is up.
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
		last_had_depth.store(has_depth_src);

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
		// compositor-owned retained sources so a synthetic below warps from
		// held data, valid until the next real frame refreshes it.
		if (has_depth_src and depth_reprojection_enabled and app_fresh)
		{
			// Depth rows arrive Vulkan top-first by producer construction
			// (dma-buf import chain, row 0 = top), independent of the color
			// layer's flip_y: retained rows mirror so the images stay
			// GL-ordered (row 0 = frustum bottom) whatever the color order.
			// Retained refresh also stamps the synth source clock (the
			// ticker's dead-app guard measures source age from here).
			last_retained_ns.store(commit_ns);
			retained_src_pose = src_pose;
			retained_src_fov = src_fov;
			retained_src_near = src_near_z;
			retained_src_far = src_far_z;
			reprojector.refresh_retained(
			        vk.device,
			        cmd,
			        src,
			        src_depth,
			        src_color_nrect,
			        src_depth_nrect,
			        /*flip_color=*/!flip_y,
			        /*flip_depth=*/true);
		}

		// Depth reprojection for stale app frames: the app is running slower
		// than the display (see the staleness gate above), so this frame will
		// still be on screen when the next display tick needs one. Warp the
		// retained sources to this tick and serve that INSTEAD of the stale
		// frame (at most one warp per commit). Fresh app frames are served
		// straight (client-side rotation timewarp covers them).
		if (stale_commit and has_depth_src and depth_reprojection_enabled)
		{
			xrt_space_relation head{};
			bool synth_ok = session.get_hmd().get_view_poses(
			                nullptr,
			                frame.rendering.predicted_display_time_ns,
			                XRT_VIEW_TYPE_STEREO,
			                2,
			                &head,
			                synth_fov.data(),
			                synth.poses.data()) == XRT_SUCCESS;

			// The tracker reports an unservable tick via the head relation
			// flags while eye poses fall back to identity+offsets: warping
			// to those produces garbage, so require a valid head pose and
			// otherwise skip silently (repeat instead).
			constexpr auto need_pose = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
			synth_ok = synth_ok && (head.relation_flags & need_pose) == need_pose;
			if (synth_ok)
			{
				// get_view_poses returns head-relative eye offsets in
				// poses plus the head-in-world relation (Monado device
				// contract): compose world-space targets here.
				for (int view = 0; view < 2; ++view)
				{
					xrt_relation_chain xrc{};
					xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
					m_relation_chain_push_pose_if_not_identity(&xrc, &synth.poses[view]);
					m_relation_chain_push_relation(&xrc, &head);
					m_relation_chain_resolve(&xrc, &rel);
					synth.poses[view] = rel.pose;
				}
				// The synthetic tick must postdate the app frame it
				// replaces; otherwise the warp below is meaningless.
				synth_ok = frame.rendering.predicted_display_time_ns > app_layer_ts_ns;
			}
			if (synth_ok)
			{
				// Plausibility gate (see helper): violent ticks splat
				// mostly-holes; repeat instead.
				float worst_ms = 0.f, worst_rs = 0.f;
				double dt_s = double(frame.rendering.predicted_display_time_ns - app_layer_ts_ns) / 1e9;
				if (!warp_motion_plausible(src_pose, synth.poses, dt_s, worst_ms, worst_rs))
				{
					note_warp_skip_implausible(worst_ms, worst_rs, dt_s * 1e3);
					synth_ok = false;
				}
			}
			if (synth_ok)
			{
				synth.image = acquire_image();
				if (synth.image < 0)
				{
					// Pool exhausted: degrade to repeat (throttled above).
					synth_ok = false;
				}
			}
			if (synth_ok)
			{
				for (int view = 0; view < 2 && synth_ok; ++view)
				{
					// Target uses the fresh tracker FOV (not the
					// render-time one): identical unless the headset
					// mutates FOV mid-session.
					synth_ok = reprojector.reproject(
					        cmd,
					        view,
					        src_pose[view],
					        src_fov[view],
					        src_near_z[view],
					        src_far_z[view],
					        synth.poses[view],
					        synth_fov[view]);
				}
				if (!synth_ok and synth.image >= 0)
				{
					// reproject() only fails on degenerate frustum input
					// (non-finite or inverted near/far/FOV): throttled, since
					// a bad upstream header would fail every tick.
					static std::atomic<uint64_t> last_degenerate_ns{0};
					uint64_t degenerate_now_ns = uint64_t(os_monotonic_get_ns());
					if (degenerate_now_ns - last_degenerate_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
					{
						last_degenerate_ns.store(degenerate_now_ns, std::memory_order_relaxed);
						U_LOG_W("compositor: warp rejected (degenerate frustum near=%.4f/%.4f far=%.1f/%.1f, repeat instead)",
						        src_near_z[0], src_near_z[1], src_far_z[0], src_far_z[1]);
					}
					images[synth.image].busy = false;
					synth.image = -1;
				}
			}
			have_synth = synth_ok;
		}
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

		if (session.get_info().eye_gaze)
		{
			auto now = os_monotonic_get_ns();
			session.add_tracking_request(device_id::EYE_GAZE, frame.rendering.desired_present_time_ns, now, now);
		}
		// On a successful replacement warp the real (stale repeat) image is
		// dropped below: skip foveating into it (the warp target is foveated
		// instead).
		warp_replaces_real = have_synth;
		if (!warp_replaces_real)
		{
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
		}

		if (have_synth)
		{
			// Synthetic frames sample the warped targets (full-eye, always
			// GL-ordered by construction, hence flip_y = true). The
			// replacement reuses this tick's identity: never consume an
			// extra pacer tick (that would desync the display cadence).
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
			auto synth_foveation = foveation.foveate(
			        vk.device,
			        cmd,
			        images[synth.image].view_y,
			        images[synth.image].view_cbcr,
			        true,
			        reprojector.get_views(),
			        synth_rect,
			        synth_fov,
			        view_info.alpha);

			images[synth.image].frame_index = uint64_t(frame.rendering.id);
			images[synth.image].view_info = {
			        .display_time = session.get_offset().to_headset(frame.rendering.predicted_display_time_ns),
			        .alpha = view_info.alpha,
			};
			for (int view = 0; view < 2; ++view)
			{
				images[synth.image].view_info.pose[view] = xrt_cast(synth.poses[view]);
				images[synth.image].view_info.fov[view] = xrt_cast(synth_fov[view]);
				images[synth.image].view_info.foveation[view] = synth_foveation[view];
			}
		}

		if (!warp_replaces_real)
			append_encoder_barriers(image_barriers, i, view_info);
		if (have_synth)
			append_encoder_barriers(image_barriers, synth.image, images[synth.image].view_info);

		cmd.pipelineBarrier2({
		        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
		        .pImageMemoryBarriers = image_barriers.data(),
		});
		cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 3);

		cmd.end();

		sem_value_now = submit_cmd_buffer(cmd);
		vk::SemaphoreSubmitInfo sem_info{
		        .semaphore = *sem,
		        .value = sem_value_now,
		        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
		};

		pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
		present_info = pacer.present_to_info(frame.rendering.desired_present_time_ns);

		// Record each encoder's slot on the queued frame; encode() consumes
		// exactly the recorded slot, so present/encode stay paired across
		// exceptions, drops and skips. (Slots hoisted above the try.)
		// Release the record lock before the bounded slot waits: present
		// touches only per-image state (already exclusive via busy) and
		// per-encoder state (own locks), so holding commit_mutex across
		// up to 3 s of slot waits would park the synth ticker behind a
		// wedged encoder. Submit order is already fixed via the queue
		// mutex inside submit_cmd_buffer.
		commit_lock.unlock();
		if (warp_replaces_real)
		{
			// The warped frame IS this tick's output: drop the stale
			// repeat and present the warp with this tick's frame id
			// (same blocking semantics as a real present).
			images[i].busy = false;
			if (!present_to_encoders(synth.image, images[synth.image].view_info, present_info.frame_id, sem_info, synth_slots))
			{
				// Every present hit the 1 s slot bound (throttled
				// above): drop the tick rather than queue a frame no
				// stream can encode.
				images[synth.image].busy = false;
				comp_frame_clear_locked(&frame.rendering);
				return XRT_SUCCESS;
			}
		}
		else
		{
			if (!present_to_encoders(i, view_info, present_info.frame_id, sem_info, real_slots))
			{
				images[i].busy = false;
				comp_frame_clear_locked(&frame.rendering);
				return XRT_SUCCESS;
			}
		}
	}
	catch (...)
	{
		// A record, submit or present threw: anything acquired above
		// (images, recorded slots) would never be encoded, so release it
		// instead of leaking it busy. The encoder already released its
		// own slot and logged; convert to an error return here
		// (exceptions must not cross the C ABI out of layer_commit).
		for (size_t ei = 0; ei < encoders.size(); ++ei)
		{
			if (real_slots[ei] >= 0)
				encoders[ei]->release_slot(uint8_t(real_slots[ei]));
			if (synth_slots[ei] >= 0)
				encoders[ei]->release_slot(uint8_t(synth_slots[ei]));
		}
		images[i].busy = false;
		if (synth.image >= 0)
			images[synth.image].busy = false;
		comp_frame_clear_locked(&frame.rendering);
		if (commit_lock.owns_lock())
			commit_lock.unlock();
		return XRT_ERROR_VULKAN;
	}

	if (warp_replaces_real)
		queue_encode(synth.image, synth_slots);
	else
		queue_encode(i, real_slots);
	// This output serves the frame's predicted tick: the ticker treats
	// ticks near a queued output as covered (nearest-pick duality with
	// the client's frame selection).
	last_output_ns.store(uint64_t(frame.rendering.predicted_display_time_ns));

	// End of the serialized region: everything below touches only
	// commit-private or self-synchronizing state (timeline wait, query
	// read, garbage collect). The record lock was already released
	// before present; release here too if a throw path still holds it.
	if (commit_lock.owns_lock())
		commit_lock.unlock();

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_value_now,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		// Bounded (1 s) by construction: a wedged GPU slows the commit
		// thread to ~1 Hz instead of parking it; the in-flight frame
		// still completes (or not) through the normal encode path, so
		// busy/slot accounting is untouched here. Throttled: a wedge
		// would otherwise log every tick.
		static uint64_t last_sem_timeout_ns = 0;
		uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns - last_sem_timeout_ns > 5'000'000'000ull)
		{
			last_sem_timeout_ns = now_ns;
			U_LOG_IFL_W(log_level, "compositor timeout");
		}
	}
	else
	{
		// Never park the commit thread on the GPU: poll with
		// availability and give up after a bounded wait (20 x 50 us).
		// A wedged GPU then costs timing data for the tick, not a
		// wedged server.
		constexpr int query_polls = 20;
		constexpr auto query_poll_step = std::chrono::microseconds(50);
		for (int q = 0; q < query_polls; ++q)
		{
			auto [res, ts] = query_pool.getResults<uint64_t>(
			        0,
			        4,
			        8 * sizeof(uint64_t),
			        2 * sizeof(uint64_t),
			        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
			if (res != vk::Result::eSuccess || ts.size() < 8)
				break;
			// Values at even indices, availability flags at odd.
			if ((ts[1] & ts[3] & ts[5] & ts[7]) == 0)
			{
				std::this_thread::sleep_for(query_poll_step);
				continue;
			}
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[2] - ts[0]) * period / 1e3);
			warp_times.add((ts[4] - ts[2]) * period / 1e3);
			foveation_times.add((ts[6] - ts[4]) * period / 1e3);
			break;
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

void compositor::append_encoder_barriers(
        beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> & barriers,
        int image_idx,
        const to_headset::video_stream_data_shard::view_info_t & info)
{
	for (auto & encoder: encoders)
	{
		if (encoder->stream_idx == 2 and not info.alpha)
			continue;
		else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
		{
			barriers.push_back(
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
}

uint64_t compositor::submit_cmd_buffer(vk::raii::CommandBuffer & cmd)
{
	vk::CommandBufferSubmitInfo cmd_info{
	        .commandBuffer = cmd,
	};
	std::unique_lock lock{vk.queue.mutex};
	uint64_t value = sem_value.fetch_add(1) + 1;
	vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};
	vk.queue.queue.submit2(vk::SubmitInfo2{
	        .commandBufferInfoCount = 1,
	        .pCommandBufferInfos = &cmd_info,
	        .signalSemaphoreInfoCount = 1,
	        .pSignalSemaphoreInfos = &sem_info,
	});
	last_submit_value.store(value, std::memory_order_relaxed);
	return value;
}

// Host-visible UBO / descriptor writes below must not land while the GPU
// still reads them from the other path's submit (recording is serialized
// by commit_mutex, execution is not). Bounded wait; on timeout the
// caller skips its warp (plain repeat / dropped tick) instead of
// corrupting in-flight work.
bool compositor::wait_for_last_submit(uint64_t timeout_ns)
{
	uint64_t v = last_submit_value.load(std::memory_order_relaxed);
	if (v == 0)
		return true;
	return vk.device.waitSemaphores(
	               vk::SemaphoreWaitInfo{
	                       .semaphoreCount = 1,
	                       .pSemaphores = &*sem,
	                       .pValues = &v,
	               },
	               timeout_ns) == vk::Result::eSuccess;
}

bool compositor::present_to_encoders(
        int image_idx,
        const to_headset::video_stream_data_shard::view_info_t & info,
        int64_t frame_id,
        const vk::SemaphoreSubmitInfo & sem_info,
        std::array<int8_t, 3> & slots)
{
	// Record each encoder's slot on the queued frame; encode() consumes
	// exactly the recorded slot, so present/encode stay paired across
	// exceptions, drops and skips.
	bool presented_any = false;
	for (size_t ei = 0; ei < encoders.size(); ++ei)
	{
		auto & encoder = encoders[ei];
		if (encoder->stream_idx == 2 and not info.alpha)
			continue;
		slots[ei] = int8_t(encoder->present_image(
		        images[image_idx].image,
		        sem_info,
		        frame_id));
		presented_any |= slots[ei] >= 0;
	}
	return presented_any;
}

void compositor::synth_work(std::stop_token tok)
{
	// Lead and staleness bounds (monotonic ns). The lead starts work
	// early enough to present in time for the tick: pacer-learned
	// pipeline latency + a warp estimate + a small margin.
	// Overestimating only adds constant latency (the client holds early
	// frames); the retained-age bound stops synthesis from a long-dead
	// app (static scene repeats instead, which is correct).
	static constexpr uint64_t kSynthWarpEstNs = 3'000'000;
	static constexpr uint64_t kSynthMarginNs = 1'000'000;
	static constexpr uint64_t kSynthMaxRetainedAgeNs = 2'000'000'000ull;

	// Carried grid position: consumed one tick per iteration so the
	// loop can never spin on the same tick (each pass either sleeps
	// to its tick or resyncs forward past missed ones).
	uint64_t next_tick_ns = 0;
	uint64_t last_period_ns = 0;

	while (!tok.stop_requested())
	{
		uint64_t period_ns = pacer.get_frame_duration();
		if (period_ns == 0)
		{
			std::unique_lock lock{synth_mutex};
			synth_cv.wait_for(lock, tok, std::chrono::milliseconds(100), [] { return false; });
			continue;
		}
		if (period_ns != last_period_ns)
		{
			// Refresh rate changed: re-seed the grid on the new period
			// instead of walking the old spacing into the new cadence.
			next_tick_ns = 0;
			last_period_ns = period_ns;
		}
		uint64_t now_ns = uint64_t(os_monotonic_get_ns());
		if (next_tick_ns <= now_ns)
			next_tick_ns = uint64_t(pacer.next_display_tick_ns(int64_t(now_ns)));
		uint64_t tick_ns = next_tick_ns;
		next_tick_ns += period_ns;
		uint64_t lead_ns = uint64_t(pacer.present_lead_ns()) + kSynthWarpEstNs + kSynthMarginNs;
		uint64_t wake_ns = tick_ns > now_ns + lead_ns ? tick_ns - lead_ns : now_ns;
		{
			std::unique_lock lock{synth_mutex};
			synth_cv.wait_for(lock, tok, std::chrono::nanoseconds(wake_ns > now_ns ? wake_ns - now_ns : 0), [] { return false; });
		}
		if (tok.stop_requested())
			return;
		now_ns = uint64_t(os_monotonic_get_ns());

		// No output within half a period of this tick means the tick is
		// uncovered (nearest-pick duality with the client's frame
		// selection: an output that close IS this tick's frame).
		// Commits stamp predicted display times (= grid + learned render
		// latency) while ticks live on the unshifted grid: subtract the
		// learned latency so the comparison is grid-vs-grid. Without this
		// a learned latency near half a period systematically covers or
		// uncovers every tick.
		int64_t half_period_ns = int64_t(period_ns / 2);
		int64_t render_bias_ns = pacer.learned_render_latency_ns();
		int64_t cover_dt = int64_t(tick_ns) - (int64_t(last_output_ns.load()) - render_bias_ns);
		bool covered = cover_dt <= half_period_ns && cover_dt >= -half_period_ns;
		uint64_t last_retained = last_retained_ns.load();
		bool retained_fresh = last_retained != 0 && now_ns - last_retained <= kSynthMaxRetainedAgeNs;
		if (covered || !retained_fresh || !depth_reprojection_enabled || !last_had_depth.load())
			continue;
		if (!session.connected() || !session.get_offset())
			continue;

		int synth_image = -1;
		std::array<xrt_pose, 2> synth_poses{};
		std::array<xrt_fov, 2> synth_fov{};
		int64_t synth_id = -1;
		bool have_synth = false;
		std::array<int8_t, 3> synth_slots{-1, -1, -1};
		try
		{
			// Opportunistic lock: a commit in flight has absolute priority
			// on the pool (skipping one synth is always cheaper than
			// dropping its fresh frame). No waiting here.
			std::unique_lock commit_lock{commit_mutex, std::try_to_lock};
			if (!commit_lock.owns_lock())
				continue;
			// Re-verify under lock: an EndFrame may have committed (and
			// queued) while we slept.
			int64_t cover_dt2 = int64_t(tick_ns) - (int64_t(last_output_ns.load()) - render_bias_ns);
			bool covered2 = cover_dt2 <= half_period_ns && cover_dt2 >= -half_period_ns;
			bool ok = !covered2;
			// Shared upload state (UBOs, descriptor sets) must be idle
			// before recording: the commit path's submit may still be on
			// the GPU (recording is serialized, execution is not).
			// Bounded by one period; on timeout skip the tick instead of
			// corrupting in-flight work. This also covers the command-pool
			// reset below (the pool is only reset after this wait).
			if (ok && !wait_for_last_submit(period_ns))
				ok = false;
			if (ok)
				ok = reprojector.has_retained();
			xrt_space_relation head{};
			if (ok)
			{
				if (session.get_hmd().get_view_poses(
				            nullptr,
				            int64_t(tick_ns),
				            XRT_VIEW_TYPE_STEREO,
				            2,
				            &head,
				            synth_fov.data(),
				            synth_poses.data()) != XRT_SUCCESS)
					ok = false;
			}
			constexpr auto need_pose = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
			if (ok && (head.relation_flags & need_pose) != need_pose)
				ok = false;
			if (ok)
			{
				for (int view = 0; view < 2; ++view)
				{
					xrt_relation_chain xrc{};
					xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
					m_relation_chain_push_pose_if_not_identity(&xrc, &synth_poses[view]);
					m_relation_chain_push_relation(&xrc, &head);
					m_relation_chain_resolve(&xrc, &rel);
					synth_poses[view] = rel.pose;
				}
			}
			if (ok)
			{
				// The tick must postdate the retained frame: warping
				// backward is never meaningful (guards clock confusion).
				ok = tick_ns > last_retained_ns.load();
			}
			if (ok)
			{
				// Plausibility gate (see helper): violent ticks splat
				// mostly-holes; skip the tick instead.
				float worst_ms = 0.f, worst_rs = 0.f;
				double dt_s = double(tick_ns - last_retained_ns.load()) / 1e9;
				if (!warp_motion_plausible(retained_src_pose, synth_poses, dt_s, worst_ms, worst_rs))
				{
					note_warp_skip_implausible(worst_ms, worst_rs, dt_s * 1e3);
					ok = false;
				}
			}
			if (ok)
			{
				synth_image = acquire_image();
				if (synth_image < 0)
					ok = false; // Pool momentarily full (commit in flight): skip silently.
			}

			if (ok)
			{
				synth_pool.reset();
				synth_cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

				// Fresh or recycled images start UNDEFINED (same as the commit
				// path: UNDEFINED discards, valid for any actual layout).
				std::array image_barrier{
				        vk::ImageMemoryBarrier2{
				                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
				                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
				                .oldLayout = vk::ImageLayout::eUndefined,
				                .newLayout = vk::ImageLayout::eGeneral,
				                .image = images[synth_image].image,
				                .subresourceRange = {
				                        .aspectMask = vk::ImageAspectFlagBits::eColor,
				                        .levelCount = 1,
				                        .layerCount = images[synth_image].image.info().arrayLayers,
				                },
				        },
				};
				synth_cmd.pipelineBarrier2({
				        .imageMemoryBarrierCount = uint32_t(image_barrier.size()),
				        .pImageMemoryBarriers = image_barrier.data(),
				});
			}
			if (ok)
			{
				for (int view = 0; view < 2 && ok; ++view)
				{
					ok = reprojector.reproject(
					        synth_cmd,
					        view,
					        retained_src_pose[view],
					        retained_src_fov[view],
					        retained_src_near[view],
					        retained_src_far[view],
					        synth_poses[view],
					        synth_fov[view]);
				}
				if (!ok && synth_image >= 0)
				{
					// reproject() only fails on degenerate frustum input
					// (non-finite or inverted near/far/FOV): throttled, since
					// a bad upstream header would fail every tick.
					static std::atomic<uint64_t> last_degenerate_ns{0};
					uint64_t degenerate_now_ns = uint64_t(os_monotonic_get_ns());
					if (degenerate_now_ns - last_degenerate_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
					{
						last_degenerate_ns.store(degenerate_now_ns, std::memory_order_relaxed);
						U_LOG_W("compositor: synth warp rejected (degenerate frustum near=%.4f/%.4f far=%.1f/%.1f, skipping tick)",
						        retained_src_near[0], retained_src_near[1], retained_src_far[0], retained_src_far[1]);
					}
					images[synth_image].busy = false;
					synth_image = -1;
				}
			}
			if (ok)
			{
				synth_id = pacer.predict_synth(int64_t(tick_ns), int64_t(tick_ns));
				images[synth_image].frame_index = uint64_t(synth_id);

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
				// Load once: the blend mode is published per commit and only
				// changes on app layer switches, so a torn read would only
				// mix one frame's worth of alpha handling.
				const bool alpha_blend = last_alpha_blend.load();
				auto synth_foveation = foveation.foveate(
				        vk.device,
				        synth_cmd,
				        images[synth_image].view_y,
				        images[synth_image].view_cbcr,
				        true, // warped targets are always GL-ordered
				        reprojector.get_views(),
				        synth_rect,
				        synth_fov,
				        alpha_blend);

				images[synth_image].view_info = {
				        .display_time = session.get_offset().to_headset(int64_t(tick_ns)),
				        .alpha = alpha_blend,
				};
				for (int view = 0; view < 2; ++view)
				{
					images[synth_image].view_info.pose[view] = xrt_cast(synth_poses[view]);
					images[synth_image].view_info.fov[view] = xrt_cast(synth_fov[view]);
					images[synth_image].view_info.foveation[view] = synth_foveation[view];
				}


				beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> image_barriers;
				append_encoder_barriers(image_barriers, synth_image, images[synth_image].view_info);
				synth_cmd.pipelineBarrier2({
				        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
				        .pImageMemoryBarriers = image_barriers.data(),
				});

				synth_cmd.end();
				have_synth = true;
			}
		if (!have_synth)
			continue;
		// Submit under the record lock (like the commit path): this
		// publishes last_submit_value before any other path can pass
		// its pre-record wait, so no record can start against
		// in-flight upload state. Submit only takes the queue mutex
		// (non-blocking). The slow bounded slot waits in
		// present_to_encoders run unlocked below.
		uint64_t sem_value_now = submit_cmd_buffer(synth_cmd);
		const vk::SemaphoreSubmitInfo sem_info{
		        .semaphore = *sem,
		        .value = sem_value_now,
		        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
		};
		commit_lock.unlock();
		if (!present_to_encoders(synth_image, images[synth_image].view_info, synth_id, sem_info, synth_slots))
		{
			images[synth_image].busy = false;
			continue;
		}
		queue_encode(synth_image, synth_slots);
		last_output_ns.store(tick_ns);
		}
		catch (...)
		{
			// Same guarantee as the commit path: a record, submit or
			// present throw must release what was acquired, never the
			// thread (a dead ticker stops all synthesis silently). The
			// encoder already released its own slot and logged.
			for (size_t ei = 0; ei < encoders.size(); ++ei)
				if (synth_slots[ei] >= 0)
					encoders[ei]->release_slot(uint8_t(synth_slots[ei]));
			if (synth_image >= 0)
				images[synth_image].busy = false;
			continue;
		}
	}
}

void compositor::queue_encode(int image, const std::array<int8_t, 3> & slots)
{
	std::lock_guard lock(encode_mutex);
	encode_queue.push_back({image, slots});
	// Bounded in practice by images: at most 2 frames queued per commit
	// while the pool holds 4; acquire_image drops past it. Never abort
	// on overflow in any build — drop the oldest instead (its image was
	// already exclusive-busy, so release it back to the pool).
	if (encode_queue.size() > images.size())
	{
		static uint64_t last_overflow_ns = 0;
		uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns - last_overflow_ns > 5'000'000'000ull)
		{
			last_overflow_ns = now_ns;
			U_LOG_W("compositor: encode queue overflow, dropping oldest");
		}
		auto old = std::move(encode_queue.front());
		encode_queue.pop_front();
		for (size_t ei = 0; ei < encoders.size(); ++ei)
			if (old.slots[ei] >= 0)
				encoders[ei]->release_slot(uint8_t(old.slots[ei]));
		images[old.image].busy = false;
	}
	encode_cv.notify_one();
}

void compositor::encoder_work(std::stop_token tok)
{
	while (true)
	{
		queued_frame req;
		{
			std::unique_lock lock(encode_mutex);
			// Bounded stop-aware wait: an idle worker still wakes
			// regularly instead of sleeping forever; stop still exits
			// promptly.
			encode_cv.wait_for(lock, tok, std::chrono::milliseconds(100), [&] { return not encode_queue.empty(); });
			if (tok.stop_requested())
				return;
			if (encode_queue.empty())
				continue;
			req = std::move(encode_queue.front());
			encode_queue.pop_front();
		}

		assert(req.image >= 0 and req.image < images.size());
		if (req.image < 0 or req.image >= int(images.size()))
		{
			static uint64_t last_bad_ns = 0;
			uint64_t now_ns = os_monotonic_get_ns();
			if (now_ns - last_bad_ns > 5'000'000'000ull)
			{
				last_bad_ns = now_ns;
				U_LOG_W("compositor: bad encode image %d, dropping", req.image);
			}
			continue;
		}
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
		catch (...)
		{
			U_LOG_W("encode error: unknown exception");
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
        synth_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        synth_cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *synth_pool, .commandBufferCount = 1})[0])},
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
	U_LOG_I("depth warp %s (source-reverse-z %s)",
	        depth_reprojection_enabled ? "enabled" : "disabled",
	        configuration().reprojection.source_reverse_z ? "true" : "false");

	// Start the threads after everything is initialized. The synth
	// ticker is declared after the encoder thread so it stops (and
	// joins) first: it produces frames, the worker drains them.
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
	synth_thread = std::jthread{[&](std::stop_token t) { synth_work(t); }};
}

compositor::~compositor()
{
	u_var_remove_root(this);
	synth_thread.request_stop();
	synth_cv.notify_all();
	synth_thread.join();
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
	if (hz <= 0)
		return;
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	pacer.set_frame_duration(U_TIME_1S_IN_NS / hz);
	// Display ticks moved: forget the last app timestamp so the next
	// commit counts as fresh, and the arrival spacing so it never counts
	// as stale (no spurious replacement warp). Also forget retained
	// freshness: the next frames may come from a different scene.
	last_app_layer_ts.store(-1);
	last_commit_ns.store(0);
	warp_armed_stale.store(false);
	last_had_depth.store(false);
	last_retained_ns.store(0);
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
	last_app_layer_ts.store(-1);
	last_commit_ns.store(0);
	warp_armed_stale.store(false);
	last_had_depth.store(false);
	last_retained_ns.store(0);
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
