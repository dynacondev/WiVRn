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
#include "render/debug_lines.h"
#include "render/feather_mask.h"
#include "render/imgui_impl.h"
#include "render/passthrough_mesh.h"
#include "scene.h"
#include "scenes/input_profile.h"
#include "stream_defoveator.h"
#include "utils/thread_safe.h"
#include "wifi_lock.h"
#include "wivrn_client.h"
#include "wivrn_packets.h"
#include "xr/board_solver.h"
#include "xr/fiducial_filter.h"
#include "xr/marker_tracker.h"
#include "xr/space.h"
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
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
	// Retained shared mask-pipeline cache: keeps the driver cache built
	// by warm_pipelines() alive from first video frame until session end
	// (renderers alone can't retain it — they don't exist yet at warm
	// time, and the lookup map holds weak refs only).
	std::shared_ptr<vk::raii::PipelineCache> mask_pipe_cache;

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
		passthrough,
		theme,
		system,
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

	XrTime running_application_req = 0;
	thread_safe<to_headset::running_applications> running_applications;

	// Fiducial tracking markers (Quest only, see docs/configuration.md).
	// Latest map from the server; model files cached under
	// application::get_config_path() / "fiducial_models" / <hash>.glb.
	thread_safe<to_headset::fiducial_map> fiducial_map;
	struct fiducial_download
	{
		uint32_t chunk_count = 0;
		std::unordered_map<uint32_t, std::vector<std::byte>> chunks;
	};
	// Guarded by network thread only (all handlers run there)
	std::unordered_map<std::string, fiducial_download> fiducial_downloads;
	static std::filesystem::path fiducial_model_path(const std::string & hash);

	// One runtime tracker per distinct (marker size, static) group, shared
	// across fiducials (the marker size is context-global, payloads match
	// client-side). Synced from the map every frame; stale trackers are
	// destroyed. Render thread only (all trackers run there).
	std::map<std::string, xr::marker_tracker> fiducial_trackers;
	std::string last_tracker_key; // group fingerprint; re-sync on change
	std::string last_frame_key;   // map fingerprint for the fused poses; reset on change
	// Set once: the runtime hides spatial tracking without its grant.
	bool marker_support_logged = false;

	// One board hypothesis (vote) per (fiducial, marker, entity):
	// observed tag pose plus the marker's size, offset and latch time.
	// ALL markers of a fiducial vote; board_solver fuses each fiducial's
	// votes into one pose. Rebuilt every frame; render thread only.
	struct fiducial_vote
	{
		std::string fiducial_id;
		std::string payload;
		XrPosef observed{{0, 0, 0, 1}, {0, 0, 0}};
		XrExtent2Df extents{0, 0}; // measured tag bounds (gizmo boxes, cutout windows)
		float tag_size_m = 0;
		std::array<float, 3> offset_pos{0, 0, 0}; // EFFECTIVE marker->board
		std::array<float, 4> offset_quat{0, 0, 0, 1}; // xyzw (authored tag-in-board, inverted at assembly)
		XrTime latch_time = 0; // track (re)latch time, 0 = unknown (settled)
		XrTime time = 0; // sighting time (vote dedupe: newest wins)
		bool novel = false; // fresh detector output (filter ingests these)
	};
	std::vector<fiducial_vote> fiducial_votes;

	// Fused board pose per fiducial id: live solution when votes exist,
	// held (frozen) otherwise. The robust-Euro filter smooths the fused
	// poses over time; consumers read pose, never the ephemeral
	// sightings. Render thread only.
	struct board_fused
	{
		XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrTime time = 0; // last live solution (stale age = now - time)
		bool live = false; // solved this frame
		float rms_mm = 0, rms_deg = 0;
		double solve_us = 0;
		int lm_iters = 0;
		int tags_visible = 0, tags_used = 0;
		bool single = true;
		std::vector<xr::board_tag_stat> tag_stats;
		std::vector<xr::board_vote> last_votes; // accepted-solve inputs, same order as tag_stats
		xr::fiducial_filter filter; // post-resolution temporal smoothing
		std::string tuning_key; // filter tuning source; re-configure on change
		uint64_t samples_ingested = 0;
		XrTime last_predicted = 0; // last advance time (advance dt source)
	};
	std::map<std::string /*fiducial id*/, board_fused> board_poses;
	// Warm starts for the solver (fiducial id -> last smoothed pose).
	// Solver init only, not output smoothing.
	std::map<std::string, XrPosef> board_warm_start;

	// Single fused placement per object. Objects persist once placed
	// (map wipe excepted): the pose follows the first referenced
	// fiducial with a pose, live or held, every frame.
	struct fused_placement
	{
		bool placed = false; // any fused solution ever
		XrPosef world_pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrVector3f world_scale{1, 1, 1};
		XrTime last_solve = 0; // last live frame (stale age)
	};
	struct passthrough_object_state
	{
		to_headset::passthrough_object def; // snapshot from the map
		// Model geometry, uploaded once per content hash.
		passthrough_mesh::triangle_soup soup;
		std::string soup_hash;
		bool soup_ready = false;
		size_t vertex_count = 0;
		size_t triangle_count = 0;
		std::string status = "waiting for fiducial map";
		fused_placement fused;
	};
	std::map<std::string /*object id*/, passthrough_object_state> passthrough_objects;
	// Live in-headset surface-projection overrides (Passthrough tab,
	// session-scoped): presence wins over the map flag. Render thread
	// only (GUI runs there too).
	std::map<std::string /*object id*/, bool> projected_overrides;
	// Object ids whose projected instance upload failed (runtime refused
	// the mesh): skip retry so a persistent failure can't spam every
	// frame. Cleared on map change with everything else.
	std::set<std::string> projected_failed;

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

	// Per-frame passthrough accumulators (milliseconds / counts), filled
	// by update_fiducial_passthrough and the mask record loop below, then
	// sampled by accumulate_metrics into global_metrics.
	double fid_detect_ms = 0, fid_solve_ms = 0, fid_place_ms = 0;
	int fid_votes = 0, fid_tags = 0;
	double mask_raster_ms = 0, mask_erode_ms = 0, mask_blur_ms = 0, mask_gizmo_ms = 0;
	int mask_draws = 0;

	struct gpu_timestamps
	{
		float gpu_time = 0;
		// Query plan (all stamps written unconditionally at fixed
		// command-buffer points, TopOfPipe except the closing one):
		// q0 frame top, q1 frame end (BottomOfPipe), q2 after
		// defoveate, q3 at mask-phase start. Spans: frame = q1-q0,
		// defoveate = q2-q0, gizmos = q3-q2, mask = q1-q3. TopOfPipe
		// stamps don't drain prior fragment work, so a late producer
		// inflates the *following* span — read spikes by span, not as
		// exact per-pass costs.
		float defoveate_gpu_time = 0;
		float gizmo_gpu_time = 0;
		float mask_gpu_time = 0;
	};

	struct global_metric
	{
		float gpu_time;
		float cpu_time = 0;
		float bandwidth_rx = 0;
		float bandwidth_tx = 0;
		// Passthrough timings (seconds, sampled per frame): fiducial
		// detection/tracking, board solve + smoothing, placement +
		// instance updates, then mask raster/erode/blur, gizmos and the
		// mask GPU span. Counts are unitless.
		float track_ms = 0;
		float solve_ms = 0;
		float place_ms = 0;
		float mask_raster_ms = 0;
		float mask_erode_ms = 0;
		float mask_blur_ms = 0;
		float gizmo_ms = 0;
		float mask_gpu_ms = 0;
		float defoveate_gpu_ms = 0;
		float gizmo_gpu_ms = 0;
		float feat_votes = 0;
		float feat_tags = 0;
		float mask_draws = 0;
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

	// Mask stacks grouped by (feather-px, union): union A holds the
	// surface-projected content (drives the projected layer), union B
	// the rest (drives the flat layer). The single dst-alpha
	// channel carries one union at a time: A is consumed by projected,
	// then B REPLACEs the alpha (color preserved) and is consumed by
	// flat, so flat wins overlaps. Render thread only.
	struct mask_group
	{
		float feather_px = 24;
		// True = union A (projected), false = union B (flat).
		bool projected = false;
		std::unique_ptr<feather_mask_renderer> renderer;
		// Dedicated member swapchain (explicit acquire/release pairing,
		// never the shared pool).
		xr::swapchain swapchain;
		bool acquired = false;
		bool wait_warned = false; // image-wait timeout already reported
		XrExtent2Di extent{0, 0}; // submitted size (tier-selected)
		bool active = false; // submitting this frame
		bool had_draws = false; // draws present last frame (edge forensics)
		XrTime last_draws_log = 0; // throttle for the edge log below
		// Uploaded object soups by object id (hash compare avoids
		// re-upload; renderer mirrors).
		std::map<std::string, std::string> mesh_hashes;
	};
	// Mask stacks grouped by (feather-px, union): union A holds the
	// surface-projected objects (drives the projected layer), union B
	// the rest (drives the flat layer). Same feather value with mixed
	// flags is two groups (two swapchains/blur stacks). The single
	// dst-alpha channel carries one union at a time: A is consumed by
	// projected, then B REPLACEs the alpha (color preserved) and is
	// consumed by flat, so flat wins overlaps. Render thread only.
	// Exact float equality groups identical parsed values; the bool
	// splits projected (A) from flat (B). Iteration order is
	// feather-ascending within each union; submit does one pass per
	// union (A then B).
	std::map<std::pair<float, bool>, mask_group> mask_groups;
	// Local debug cutout group (union B): QR windows punched at full
	// alpha. Standalone member, not in the map: debug never keys off
	// server feather values. Claimed only while cutouts exist and no B
	// group does (real B groups punch cutouts themselves).
	mask_group debug_group;
	// Set when the record phase built marker-window cutouts this frame
	// (they punch camera with no geometry to map it onto, so the flat
	// feed must stay submitted). Read by the submit phase below.
	bool cutouts_punched = false;

	// QR debug gizmos (Passthrough tab, session-scoped, render thread
	// only). Raw sighting poses while visible with a 3s stale fade;
	// the orange set re-projects fused poses with authored offsets.
	bool debug_overlays = false; // master toggle
	bool debug_matched = true;   // green: configured payload
	bool debug_unmatched = true; // red: anything else sighted
	bool debug_axes = true;      // RGB XYZ tripod on sighted tags
	bool debug_corrected = true; // orange: fused-corrected tag boxes
	bool debug_origin = true;    // larger tripod + center square at board origins
	bool debug_smoothed = true;  // twin tripod (smaller, cyan square) at the smoothed pose
	float debug_opacity = 0.5f;  // overlay alpha, 0..1
	// Marker window cutout, mm past the marker edge punched through
	// the flat union's mask (-1 = disabled). Gated on the master toggle.
	int debug_window_mm = 50;
	size_t dbg_tris = 0, dbg_boxes = 0; // last recorded debug counts
	std::unique_ptr<debug_lines_renderer> gizmo_lines;

	// Stale-hold multiplier for debug visuals: full presence on fresh
	// data, smooth 3s fade-out on hold. Returns 0 (gone) .. 1 (fresh).
	static constexpr double kStaleFadeS = 3.0;
	static float stale_hold(XrTime now, XrTime last_seen)
	{
		if (last_seen == 0)
			return 0;
		double t = std::clamp((now - last_seen) * 1e-9 / kStaleFadeS, 0.0, 1.0);
		return (float)(1.0 - t * t * (3 - 2 * t));
	}

	void accumulate_metrics(XrTime predicted_display_time, const std::array<std::shared_ptr<wivrn::shard_accumulator::blit_handle>, decoder_count> & blit_handles, const gpu_timestamps & timestamps);
	void gui_performance_metrics();
	void gui_compact_view();
	void gui_settings(float predicted_display_period);
	void gui_bitrate_settings(float predicted_display_period);
	void gui_foveation_settings(float predicted_display_period);
	void gui_applications();
	void gui_toasts();
	void gui_passthrough();
	void gui_fiducial_status();
	void draw_gui(XrTime predicted_display_time, XrDuration predicted_display_period);

	// Eager USE_SCENE runtime-permission request (Android): manifest presence
	// makes the spatial extensions enumerable, but tracking data needs the
	// user grant. Process-once; the result feeds the status-tab hint.
	void request_spatial_permissions();
	void sync_fiducial_trackers(const to_headset::fiducial_map & map);
	void update_fiducial_passthrough(XrTime predicted_display_time);
	bool surface_projected_effective(const std::string & oid, const passthrough_object_state & ost) const;
	bool guard_k1_jump(const std::string & fid, const xr::board_solution & sol,
	                   const std::optional<XrPosef> & warm, XrTime now, double & dt_out);
	void log_board_tuning(const std::string & fid, const std::vector<xr::board_vote> & bv,
	                      const xr::board_solution & sol, const board_fused & bp,
	                      double horizon_ms, XrTime now, double dt,
	                      bool have_head, glm::vec3 head_pos);
};
} // namespace scenes
