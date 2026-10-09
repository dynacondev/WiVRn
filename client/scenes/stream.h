/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#pragma once

#include "app_launcher.h"
#include "application.h"
#include "audio/audio.h"
#include "configuration.h"
#include "decoder/shard_accumulator.h"
#include "render/feather_mask.h"
#include "render/imgui_impl.h"
#include "scene.h"
#include "scenes/input_profile.h"
#include "xr/fiducial_filter.h"
#include "stream_defoveator.h"
#include "utils/thread_safe.h"
#include "wifi_lock.h"
#include "wivrn_client.h"
#include "wivrn_packets.h"
#include "xr/marker_tracker.h"
#include "xr/space.h"
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

namespace scenes
{
class stream : public scene_impl<stream>, public std::enable_shared_from_this<stream>
{
public:
	enum class state
	{
		initializing,
		streaming,
		stalled,
		shutdown,
	};
	static const size_t image_buffer_size = 3;

	app_launcher apps;

private:
	static const size_t view_count = 2;
	static const size_t decoder_count = view_count + 1;

	struct accumulator_images
	{
		std::unique_ptr<wivrn::shard_accumulator> decoder;
		// latest frames, rolling buffer
		std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, image_buffer_size> latest_frames;

		std::shared_ptr<wivrn::shard_accumulator::blit_handle> frame(uint64_t id) const;
		bool empty() const;
	};

	wifi_lock::wifi wifi;

	// for frames inside accumulator images
	std::mutex frames_mutex;
	std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> common_frame(XrTime display_time);

	std::unique_ptr<wivrn_session> network_session;
	std::thread network_thread;
	thread_safe<to_headset::tracking_control> tracking_control{};
	std::array<std::atomic<interaction_profile>, 3> interaction_profiles; // left hand, right hand, gamepad
	std::atomic<bool> interaction_profile_changed = false;
	std::atomic<XrTime> scheduled_derived_pose = 0; // Tracking thread will compute derived pose when time is reached
	std::atomic<bool> recenter_requested = false;
	std::atomic<bool> hid_forwarding = false;
	std::atomic<XrDuration> display_time_phase = 0;
	std::atomic<XrDuration> display_time_period = 0;
	XrTime last_display_time = 0;
	std::atomic<XrDuration> real_display_period = 0;
	std::optional<std::thread> tracking_thread;

	std::shared_mutex decoder_mutex;
	std::optional<to_headset::video_stream_description> video_stream_description;
	std::array<accumulator_images, decoder_count> decoders; // Locked by decoder_mutex

	std::optional<stream_defoveator> defoveator;

	vk::raii::Fence fence = nullptr;
	vk::raii::CommandBuffer command_buffer = nullptr;

	struct haptics_action
	{
		XrAction action;
		XrPath path;
		float amplitude;
	};
	std::unordered_multimap<device_id, haptics_action> haptics_actions;
	std::vector<std::tuple<device_id, XrAction, XrActionType>> input_actions;

	std::atomic<state> state_ = state::initializing;

	void set_state(state new_state)
	{
		state prev = state_;
		if (prev == state::shutdown)
			return;

		state_.compare_exchange_strong(prev, new_state);
	}

	xr::swapchain swapchain;

	// Dedicated feather-mask swapchain (NOT from the shared get_swapchain
	// pool: pooled entries are release-all at render_end, which unpaired a
	// re-fetched handle and killed the app with CALL_ORDER_INVALID).
	// Acquired + released explicitly around the raster record each frame.
	xr::swapchain mask_swapchain;

	std::optional<audio> audio_handle;

	std::optional<xr::hand_tracker> left_hand;
	std::optional<xr::hand_tracker> right_hand;
	std::optional<input_profile> input;
	static inline const uint32_t layer_controllers = 1 << 0;
	static inline const uint32_t layer_rays = 1 << 1;

	// Size of the composition layer used for the controllers
	uint32_t width;
	uint32_t height;

	std::optional<imgui_context> imgui_ctx;
	ImTextureID wivrn_logo = 0; // wordmark logo shown in the top bar, like the lobby
	struct gui_toast
	{
		std::string content;
		bool is_urgent = false;
	};

	static bool is_interactable(stream_tab);
	bool is_gui_interactable() const;

	// settings sub-page, client-only: the wire stream_tab stays settings
	enum class settings_page
	{
		video,
		audio,
		streaming,
		post_processing,
		devices,
		tracking,
		theme,
		system,
		passthrough,
	};
	settings_page current_settings_page = settings_page::video;

	// Tab currently being displayed
	stream_tab gui_status = stream_tab::hidden;
	// Tab that we will switch to if button is pressed
	stream_tab stored_gui_status = stream_tab::applications;
	// Tab that will be displayed on next render()
	std::atomic<stream_tab> next_gui_status = stream_tab::hidden;
	float dimming = 0;

	thread_safe<std::optional<gui_toast>> gui_toast;
	std::atomic<XrTime> gui_status_last_change;

	thread_safe<std::queue<std::string>> stream_error_queue;

	XrAction plots_toggle_1 = XR_NULL_HANDLE;
	XrAction plots_toggle_2 = XR_NULL_HANDLE;
	XrAction recenter_left = XR_NULL_HANDLE;
	XrAction recenter_right = XR_NULL_HANDLE;
	XrAction gui_distance_left = XR_NULL_HANDLE;
	XrAction gui_distance_right = XR_NULL_HANDLE;
	XrAction settings_adjust = XR_NULL_HANDLE;
	XrAction foveation_distance = XR_NULL_HANDLE;
	XrAction foveation_ok = XR_NULL_HANDLE;
	XrAction foveation_cancel = XR_NULL_HANDLE;

	// Position of the GUI relative to the view space, in view space axes, used when the GUI is not interactable
	glm::vec3 head_gui_position{-0.1, -0.3, -1.2}; // Shift 10cm left by default so that the stats are centered accounting for the tab list
	glm::quat head_gui_orientation{1, 0, 0, 0};

	// Position of the GUI relative to the world space, in world space axes, used when the GUI is interactable
	glm::vec3 world_gui_position;
	glm::quat world_gui_orientation;

	bool override_foveation_enable;
	float override_foveation_pitch; // The pitch is the opposite as the height displayed in the GUI
	float override_foveation_distance;

	// Which controller is used for recentering and position of the GUI relative to the controller, in controller axes, during recentering
	std::optional<std::tuple<xr::spaces, glm::vec3, glm::quat>> recentering_context;
	void update_gui_position(xr::spaces controller, float predicted_display_period);

	// Keep a reference to the resources needed to blit the images until vkWaitForFences
	std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> current_blit_handles;

	// Video stall detection (render thread only). When the decoders stop
	// delivering frames the compositor keeps submitting an empty layer,
	// which reads as unexplained grey: surface an explicit toast instead.
	// ever_received_video latches on the first displayed frame so a slow
	// first frame doesn't false-positive.
	XrTime video_starved_since = 0;
	bool video_stall_toasted = false;
	bool ever_received_video = false;
	void update_video_stall_status(bool starved);

	// Composition diagnostics (render thread only): logged on change so a
	// grey-vs-video mystery leaves timestamps in logcat.
	bool last_use_alpha = false;
	std::atomic<bool> video_desc_received{false};
	std::atomic<XrTime> video_desc_at{0};

	XrTime running_application_req = 0;
	thread_safe<to_headset::running_applications> running_applications;

	// Fiducial-anchored passthrough meshes (ROADMAP.md Phase 1).
	// Latest map from the server; model files cached under
	// application::get_config_path() / "fiducial_models" / <hash>.glb.
	thread_safe<std::vector<to_headset::fiducial_map_entry>> fiducial_entries;
	struct fiducial_download
	{
		uint32_t chunk_count = 0;
		std::unordered_map<uint32_t, std::vector<std::byte>> chunks;
	};
	// Guarded by network thread only (all handlers run there)
	std::unordered_map<std::string, fiducial_download> fiducial_downloads;
	static std::filesystem::path fiducial_model_path(const std::string & hash);

	// Surface-projected passthrough mesh (ROADMAP.md Phase 2).
	// Phase 2 placement: the config position/orientation/scale is used
	// directly as a world-space pose (no marker yet). Phase 4 reinterprets
	// it as a marker-to-mesh offset.
	struct fiducial_passthrough_state
	{
		bool attempted = false; // upload tried at least once (no retry spam)
		bool ready = false;     // mesh live in the runtime
		bool marker_support_logged = false;
		bool calibrated = false; // mesh anchor came from calibrate_to_marker()
		XrTime calibrated_at = 0;
		// Display label resolved at calibrate time (tag, or payload when
		// untagged). Never used for matching.
		std::string calibrated_tag;
		std::string last_map_key; // map identity; a change invalidates calibration
		std::string last_key;   // fingerprint of map + cache, resets attempted
		std::string model_hash;
		std::string status = "waiting for fiducial map";
		size_t vertex_count = 0;
		size_t triangle_count = 0;
		XrPosef world_pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrVector3f world_scale{1, 1, 1};

		// Continuous mode ("continuous" map entries): auto-anchors on first
		// sighting, then robustly averages novel mesh-targets and
		// proportionally follows every frame (never snaps after the seed).
		// One-shot entries ignore all of this.
		bool continuous = false;
		xr::fiducial_filter filter;
		std::string applied_tuning_key; // filter tuning source; re-apply on change
		XrTime last_predicted = 0;
		// Diagnostics (render thread only).
		uint64_t novel_ingested = 0;
		XrTime last_novel_at = 0;
		float target_render_err_mm = 0;
		float target_render_err_deg = 0;

		// First-acquisition alpha fade (mask-blend path only: the binary
		// projected layer type has no opacity control). fade_start stamps
		// the seed (auto-anchor / calibrate / re-seed) in predicted-time
		// base; fade_dur_s tracks the entry live. 0 duration = instant.
		XrTime fade_start = 0;
		float fade_dur_s = 0;
		float fade_factor(XrTime predicted) const
		{
			if (fade_dur_s <= 0 or fade_start == 0)
				return 1;
			double t = (predicted - fade_start) * 1e-9 / fade_dur_s;
			return std::clamp(t, 0.0, 1.0);
		}

		// Feathered mask-blend state. Replaces the binary triangle-mesh
		// cutout when XR_FB_composition_layer_alpha_blend is available:
		// the calibrated mesh silhouette is rasterized into a tiny shared
		// mask layer whose upscale is the alpha-gradient feather band.
		// Feather width comes live from the calibrated map entry
		// (no recalibration needed to change it).
		passthrough_mesh::triangle_soup mask_soup;
		std::string mask_hash; // model hash the soup was built from
		std::unique_ptr<feather_mask_renderer> mask_renderer;
		bool mask_ready = false;  // soup uploaded, safe to raster
		bool mask_active = false; // submitting the mask stack this frame
		bool mask_wait_warned = false; // image-wait timeout already reported
		XrExtent2Di mask_extent{0, 0};
		float feather_px = 24;
	};
	fiducial_passthrough_state fiducial_passthrough;

	// True when a cached model + runtime support exist, i.e. the projected
	// layer should be enabled even for opaque (non-alpha) server video
	bool fiducial_passthrough_wanted();
	void update_fiducial_passthrough(XrTime predicted_display_time);
	void gui_fiducial_status();
	void gui_passthrough();

	// Eager USE_SCENE runtime-permission request (Android): manifest presence
	// makes the spatial extensions enumerable, but tracking data needs the
	// user grant. Process-once; the result feeds the Stats-tab hint.
	void request_spatial_permissions();

	// Snap the mesh to observedMarkerPose * configOffset. Requires a fresh
	// marker sighting. Object placement only: the tracking origin is never
	// touched (see calibrate_to_marker()).
	void calibrate_to_marker();

	// QR-code tracking (ROADMAP.md Phase 3, Quest runtime capability).
	// Driven on the render thread from update_fiducial_passthrough(); empty
	// until the first fiducial map arrives and the runtime supports it.
	std::optional<xr::marker_tracker> marker_tracker;

	stream(std::string server_name, scene & parent_scene);

	bool forward_hid_input(from_headset::hid::input_t, bool device_enabled);

public:
	~stream();

	static std::shared_ptr<stream> create(
	        std::unique_ptr<wivrn_session> session,
	        float guessed_fps,
	        std::string server_name,
	        scene & parent_scene);

	void render(const XrFrameState &) override;
	void on_focused() override;
	void on_unfocused() override;
	void on_xr_event(const xr::event &) override;

	bool on_input_key_down(uint8_t key_code) override;
	bool on_input_key_up(uint8_t key_code) override;
	bool on_input_mouse_move(float x, float y) override;
	bool on_input_button_down(uint8_t button) override;
	bool on_input_button_up(uint8_t button) override;
	bool on_input_scroll(float h, float v) override;

	void operator()(to_headset::crypto_handshake &&) {};
	void operator()(to_headset::pin_check_2 &&) {};
	void operator()(to_headset::pin_check_4 &&) {};
	void operator()(to_headset::handshake &&) {};
	void operator()(to_headset::server_message &&);
	void operator()(to_headset::video_stream_data_shard &&);
	void operator()(to_headset::haptics &&);
	void operator()(to_headset::timesync_query &&);
	void operator()(to_headset::tracking_control &&);
	void operator()(to_headset::feature_control &&);
	void operator()(to_headset::audio_stream_description &&);
	void operator()(to_headset::video_stream_description &&);
	void operator()(to_headset::refresh_rate_change &&);
	void operator()(to_headset::stream_tab_change &&);
	void operator()(to_headset::application_list &&);
	void operator()(to_headset::application_icon &&);
	void operator()(to_headset::running_applications &&);
	void operator()(to_headset::fiducial_map &&);
	void operator()(to_headset::fiducial_model_chunk &&);
	void operator()(audio_data &&);

	void push_blit_handle(wivrn::shard_accumulator * decoder, std::shared_ptr<wivrn::shard_accumulator::blit_handle> handle);

	void send_feedback(const wivrn::from_headset::feedback & feedback);

	state current_state() const
	{
		return state_;
	}

	// Whether the server mirrors forwarded input devices to uinput. The gamepad is also exposed
	// through OpenXR regardless, so this only affects forwarded keyboard and mouse.
	bool hid_forwarding_enabled() const
	{
		return hid_forwarding;
	}

	void exit();
	void start_application(std::string appid);

	static meta & get_meta_scene();
	std::optional<std::string> pop_stream_error();

private:
	void process_packets();
	void tracking();
	void read_actions();

	void on_interaction_profile_changed(const XrEventDataInteractionProfileChanged &);
	void send_derived_pose();

	void setup(const to_headset::video_stream_description &);
	void setup_reprojection_swapchain(uint32_t width, uint32_t height);

	vk::raii::QueryPool query_pool = nullptr;
	bool query_pool_filled = false;

	// Used for plots
	uint64_t bytes_received = 0;
	uint64_t bytes_sent = 0;
	float bandwidth_rx = 0;
	float bandwidth_tx = 0;

	struct gpu_timestamps
	{
		float gpu_time = 0;
	};

	struct global_metric
	{
		float gpu_time;
		float cpu_time = 0;
		float bandwidth_rx = 0;
		float bandwidth_tx = 0;
	};

	struct plot
	{
		std::string title;
		struct subplot
		{
			std::string title;
			float scenes::stream::global_metric::* data;
		};
		std::vector<subplot> subplots;
		const char * unit;
	};

	static const inline int size_gpu_timestamps = 1 + sizeof(gpu_timestamps) / sizeof(float);

	struct decoder_metric
	{
		// All times are in seconds relative to encode_begin
		float encode_begin;
		float encode_end;
		float send_begin;
		float send_end;
		float received_first_packet;
		float received_last_packet;
		float sent_to_decoder;
		float received_from_decoder;
		float blitted;
		float displayed;
		float predicted_display;
	};

	std::vector<global_metric> global_metrics{300};
	std::vector<std::vector<decoder_metric>> decoder_metrics;
	std::vector<float> axis_scale;
	XrTime last_metric_time = 0;
	int metrics_offset = 0;

	// Used for compact view
	float compact_bandwidth_rx = 0;
	float compact_bandwidth_tx = 0;
	float compact_cpu_time = 0;
	float compact_gpu_time = 0;

	void accumulate_metrics(XrTime predicted_display_time, const std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> & blit_handles, const gpu_timestamps & timestamps);
	void gui_performance_metrics();
	void gui_compact_view();
	void gui_settings(float predicted_display_period);
	void gui_bitrate_settings(float predicted_display_period);
	void gui_foveation_settings(float predicted_display_period);
	void gui_applications();
	void gui_toasts();
	void draw_gui(XrTime predicted_display_time, XrDuration predicted_display_period);
};
} // namespace scenes
