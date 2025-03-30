/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
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

#include "wivrn_config.h"
#include "xr/hand_tracker.h"
#include "xr/space.h"
#include "xr/swapchain.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <implot.h>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include <openxr/openxr.h>

class imgui_context
{
	struct command_buffer
	{
		vk::raii::CommandBuffer command_buffer = nullptr;
		vk::raii::Fence fence = nullptr;
	};

	struct texture_data
	{
		vk::raii::Sampler sampler;
		std::shared_ptr<vk::raii::ImageView> image_view;
		vk::raii::DescriptorSet descriptor_set;
	};

public:
	struct imgui_frame
	{
		vk::Image destination;
		vk::raii::ImageView image_view_framebuffer = nullptr;
		vk::raii::Framebuffer framebuffer = nullptr;
	};

	struct controller
	{
		XrSpace aim;
		std::pair<glm::vec3, glm::quat> offset;

		XrAction trigger; // XR_ACTION_TYPE_FLOAT_INPUT
		XrAction squeeze; // XR_ACTION_TYPE_FLOAT_INPUT
		XrAction scroll;  // XR_ACTION_TYPE_VECTOR2F_INPUT
		// TODO: thresholds?

		xr::hand_tracker * hand = nullptr;
	};

	struct controller_state
	{
		bool active = false;

		glm::vec3 aim_position = {0, 0, 0};
		glm::quat aim_orientation = {1, 0, 0, 0};

		float trigger_value = 0;
		glm::vec2 scroll_value = {0, 0};

		std::optional<ImVec2> pointer_position;
		float hover_distance = 1e10;

		bool squeeze_clicked = false;
		bool trigger_clicked = false;
		bool fingertip_hovering = false;
		bool fingertip_touching = false;
		ImGuiMouseSource source = ImGuiMouseSource_Mouse;
	};

	struct viewport
	{
		// Position of this viewport in the world
		xr::spaces space; // Must be world to allow the cursor to work
		glm::vec3 position;
		glm::quat orientation;
		glm::vec2 size;

		// Position of this viewport in the swapchain image
		glm::ivec2 vp_origin;
		glm::ivec2 vp_size;

		bool always_show_cursor = false; // Show the cursor in this viewport even if there is a modal popup elsewhere (eg. this is a virtual keyboard)

		int z_index = 0;

		ImVec2 vp_center() const
		{
			return ImVec2(vp_origin.x + int(vp_size.x / 2), vp_origin.y + int(vp_size.y / 2));
		}
	};

private:
	vk::raii::PhysicalDevice physical_device;
	vk::raii::Device & device;
	uint32_t queue_family_index;
	vk::raii::Queue & queue;

	vk::raii::Pipeline pipeline = nullptr;
	vk::raii::DescriptorPool descriptor_pool;
	vk::raii::DescriptorSetLayout ds_layout;
	vk::raii::RenderPass renderpass;
	vk::raii::CommandPool command_pool;

	std::unordered_map<ImTextureID, texture_data> textures;

	std::vector<imgui_frame> frames;
	imgui_frame & get_frame(vk::Image destination);

	std::vector<command_buffer> command_buffers;
	size_t current_command_buffer = 0;
	command_buffer & get_command_buffer()
	{
		return command_buffers[current_command_buffer];
	}

	vk::Extent2D size;
	vk::Format format;
	vk::ClearValue clear_value = {vk::ClearColorValue{0, 0, 0, 0}};

	std::vector<viewport> layers_;

	xr::swapchain & swapchain;
	int image_index;

	ImGuiContext * context;
	ImPlotContext * plot_context;
	ImGuiIO & io;

	std::vector<std::pair<controller, controller_state>> controllers;
	XrSpace world;
	size_t focused_controller = (size_t)-1;
	XrTime last_display_time = 0;

	bool button_pressed = false;
	bool fingertip_touching = false;

	ImFontGlyphRangesBuilder glyph_range_builder;
	ImVector<ImWchar> glyph_ranges;
	bool glyph_range_dirty = true;

#if WIVRN_SHOW_IMGUI_DEMO_WINDOW
	bool show_demo_window = true;
#endif

	void initialize_fonts();

	std::vector<controller_state> read_controllers_state(XrTime display_time);
	size_t choose_focused_controller(const std::vector<controller_state> & new_states) const;

public:
	imgui_context(
	        vk::raii::PhysicalDevice physical_device,
	        vk::raii::Device & device,
	        uint32_t queue_family_index,
	        vk::raii::Queue & queue,
	        std::span<controller> controllers,
	        xr::swapchain & swapchain,
	        std::vector<viewport> layers);

	~imgui_context();

	std::span<viewport> layers()
	{
		return layers_;
	}

	viewport & layer(ImVec2 position);

	void new_frame(XrTime display_time);
	std::vector<std::pair<int, std::unique_ptr<XrCompositionLayerQuad>>> end_frame();

	ImFont * large_font;
	size_t get_focused_controller() const
	{
		return focused_controller;
	}

	std::vector<std::pair<ImVec2, float>> ray_plane_intersection(const controller_state & in) const;
	void compute_pointer_position(controller_state & state);

	// Convert position from viewport coordinates to real-world
	glm::vec3 rw_from_vp(const ImVec2 & position);

	ImTextureID load_texture(const std::string & filename, vk::raii::Sampler && sampler);
	ImTextureID load_texture(const std::string & filename);
	void free_texture(ImTextureID);
	void set_current();

	void add_chars(std::string_view sv);
	bool is_modal_popup_shown() const;
};
