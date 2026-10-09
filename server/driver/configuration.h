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

#include "hostname.h"
#include "wivrn_config.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>

#include "wivrn_packets.h"

namespace wivrn
{

enum class service_publication
{
	none,
	avahi,
};

struct configuration
{
	struct encoder
	{
		std::string name;
		std::optional<wivrn::video_codec> codec;
		std::map<std::string, std::string> options;
		std::optional<std::string> device;
	};

	std::array<encoder, 3> encoders; // left, right, alpha
	std::optional<uint8_t> bit_depth;
	std::optional<std::array<float, 3>> grip_surface;
	std::vector<std::string> application;

	// Fiducial tracking markers (Quest only, see ROADMAP.md). Each
	// fiducial resolves its markers to one 6DoF frame; behavior objects
	// in passthrough[] reference fiducials by id and place instances per
	// sighted marker entity.
	struct fiducial_marker
	{
		// Exact decoded payload identifying the marker (required).
		std::string marker_data;
		float marker_size_m = 0;
		// Marker-to-fiducial offset: fiducialPose = observedMarkerPose
		// * offset. Position in meters, orientation as xyzw quaternion
		// (parsed from [rx, ry, rz] degrees).
		std::array<float, 3> position = {0, 0, 0};
		std::array<float, 4> orientation = {0, 0, 0, 1};
	};
	struct fiducial_entry
	{
		// Stable link key referenced by passthrough objects (required,
		// must be unique).
		std::string id;
		// Display-only label for the headset status UI. Never used for
		// matching or linkage.
		std::string tag;
		bool is_static = true;
		// Resolver/smoothing tuning. The multi-marker resolve and the
		// smoothing move are future work; values are carried through to
		// the per-instance filters unchanged for now.
		int window_size = 12;
		int min_samples = 4;
		float sigma_k = 3;
		float pos_gain = 24;
		float rot_gain = 24;
		float euro_min_cutoff = 0.4f;
		float euro_beta = 0.07f;
		float knee_inner_mm = 1;
		float knee_outer_mm = 5;
		float knee_inner_deg = 0.1f;
		float knee_outer_deg = 0.5f;
		std::vector<fiducial_marker> markers;
	};
	// Behavior objects placed from solved fiducial frames. Instances fan
	// out per sighted marker entity: the same QR seen twice places the
	// object twice; one object may reference several fiducials (one
	// instance per visible pairing, never fused).
	struct passthrough_object
	{
		// Behavior type. "3d-passthrough" renders the model as a
		// surface-projected passthrough cutout. Unknown types are
		// skipped with a warning (forward-compat for
		// 3d-passthrough-reversed, 3d-boundary(-reversed), 3d-depth).
		std::string type = "3d-passthrough";
		std::string id;
		std::string tag;
		// Referenced fiducial ids (dangling refs skip the object).
		std::vector<std::string> fiducial;
		std::string model_path;
		// Fiducial-to-object offset: objectPose = solvedFiducialPose *
		// offset. Uniform scale applies last, objects only.
		std::array<float, 3> position = {0, 0, 0};
		std::array<float, 4> orientation = {0, 0, 0, 1};
		float scale = 1;
		// Passthrough window feather width in screen pixels (render-only,
		// applied live). Mask stacks group by this value so each object
		// feathers independently.
		float feather_px = 24;
		// Alpha fade-in at first acquisition, milliseconds (0 = instant,
		// render-only, applied live).
		float fade_in_ms = 1000;
	};
	std::vector<fiducial_entry> fiducials;
	std::vector<passthrough_object> passthrough_objects;

	bool debug_gui = false;
	bool use_steamvr_lh = false;
	std::optional<int64_t> lh_max_extrapolation;
	std::optional<float> lh_stick_deadzone;
	bool hid_forwarding = false;
	bool tcp_only = false;
	int port = wivrn::default_port;
	std::string hostname = wivrn::hostname();
	service_publication publication = service_publication::avahi;

	// monostate: default value, string: user defined, nullptr: disabled
	std::variant<std::monostate, std::string, std::nullptr_t> openvr_compat_path;

	static void set_config_file(const std::filesystem::path &);
	static std::filesystem::path get_config_file();

	static nlohmann::json read_configuration();
	configuration();
};

std::string server_cookie();

struct headset_key
{
	std::string public_key;
	std::string name;
	std::optional<std::chrono::system_clock::time_point> last_connection;
};

std::vector<headset_key> known_keys();
void add_known_key(headset_key key);
void remove_known_key(const std::string & key);
void rename_known_key(headset_key key);
void update_last_connection_timestamp(const std::string & key);

} // namespace wivrn
