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

	// Fiducial-anchored passthrough meshes (Quest only, see ROADMAP.md).
	// Maps a QR-code marker (by exact payload string) to a glB model +
	// marker-to-mesh offset.
	struct fiducial_entry
	{
		// Display-only label for the headset status UI (correlates the
		// GUI row with this config entry). Never used for matching.
		std::string tag;
		float marker_size_m = 0;
		// Exact decoded payload identifying the marker (required).
		std::string marker_data;
		std::string model_path;
		std::array<float, 3> position = {0, 0, 0};
		std::array<float, 4> orientation = {0, 0, 0, 1};
		float scale = 1;
		// Feather width in screen pixels for the passthrough window edges.
		float feather_px = 24;
		// Tracking mode: "one-shot" (align once per Calibrate press) or
		// "continuous" (auto-anchor on sight, then smooth-follow to correct
		// drift without ever snapping). Unknown values are rejected at parse.
		std::string mode = "one-shot";
		// Maps to optimizeForStaticMarker in the QR spatial context.
		// true = stationary rig (integrate over time); false = moving
		// reference marker. Toggling recreates the client spatial context
		// (brief tracking hitch, noted here so it isn't a surprise).
		bool is_static = true;
		// Continuous-mode tuning; all optional, all live per map (a change
		// re-seeds the filter like any other map change). Shared Euro
		// cutoff/beta cover both position and orientation.
		float update_hz = 10;
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
	};
	std::vector<fiducial_entry> fiducial_map;

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
