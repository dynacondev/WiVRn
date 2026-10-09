/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
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

// Feathered passthrough window mask.
//
// Per frame (both eyes): rasterize the calibrated mesh silhouette as SDF
// seeds at reduced resolution, run a truncated Jump Flood cascade (only as
// far as the feather reaches), then composite analytically in a single
// full-resolution pass: alpha is 1 inside the silhouette, smoothstepped
// 1 -> 0 over feather-px outside it. Interiors stay pixel-exact to the
// rasterized edge; the band never erodes into the model (unlike the old
// Gaussian, whose band ran +-F/2).
//
// SDF resolution follows the band (distance error ~0.5 SDF texel stays
// negligible against it): feather <= 8 floods at half mask resolution,
// <= 64 at quarter, above at eighth. feather <= 0 keeps the exact
// hard-edge direct raster (no field work at all). The sign edge always
// resolves from the half-res coverage texture, so coarser floods cost no
// boundary quality.
//
// Swapchain images only allow COLOR_ATTACHMENT output, which is why the
// seed is a raster pass and not a buffer fill.

#include "render/passthrough_mesh.h"

#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <map>
#include <openxr/openxr.h>
#include <optional>
#include <string>
#include "vk/allocation.h"
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

class feather_mask_renderer
{
public:
	feather_mask_renderer(vk::raii::Device & device,
	                      vk::raii::PhysicalDevice & physical_device,
	                      vk::Format format);

	// (Re)upload one object's mesh buffers under its key. Synchronous
	// (device idle on replace); call when idle, e.g. on map change, not
	// per frame. Empty soup erases the entry.
	void set_mesh(const std::string & key, const passthrough_mesh::triangle_soup & soup);
	void remove_mesh(const std::string & key);

	// One silhouette draw: which uploaded mesh, per-eye transforms, and
	// the first-acquisition fade opacity.
	struct instance_draw
	{
		std::string mesh;
		std::array<glm::mat4, 2> mvp;
		float opacity = 1;
	};

	// Per-record host-side cost breakdown (steady_clock, milliseconds).
	// Filled when record() gets a non-null out-pointer; all zeros on
	// early-out paths. Permanent diagnostics: the stream scene sums these
	// per frame into the Statistics plots and a periodic logcat line.
	// GPU cost per group comes from timestamp brackets the caller writes
	// around record() (one group = one blur stack = the GPU unit of
	// interest); CPU sections here identify the host-hot stage.
	struct mask_stage_cpu
	{
		double raster_ms = 0; // SDF seed raster (or tier-0 direct raster)
		double blur_ms = 0;   // Jump Flood cascade (all iterations)
		double punch_ms = 0;  // marker-window cutout punch
		double total_ms = 0;  // whole record() body, incl. target setup
		int tier = -1;        // 0 = hard edge, else SDF downsample divisor
		size_t draws = 0;     // silhouette drawIndexed calls (both eyes)
	};

	// Record silhouettes + SDF chain for both eyes into the layers of an
	// acquired swapchain image. No-op when no mesh is set. The image must
	// be unused (UNDEFINED is fine); it is left in GENERAL for the
	// compositor. Feather selects the path (0 = hard edge raster direct,
	// >0 = SDF at half/quarter resolution, clamped to 128px);
	// rasterize=false clears only.
	// Cutouts are world-space quads (two triangles each) punched crisp
	// through the finished mask at full alpha (mask 1 = reality), sharing
	// one view-only MVP. Mask path only (the binary projected layer has
	// no alpha control to punch through).
	// Per-instance fade opacity collapses to the group minimum: the field
	// carries distance only, so the composite applies one uniform opacity
	// (identical to per-draw while a group holds a single instance).
	void record(vk::raii::CommandBuffer & cmd,
	            vk::Image image,
	            vk::Extent2D extent,
	            const std::vector<instance_draw> & draws,
	            bool rasterize,
	            float feather_px,
	            const std::vector<std::array<glm::vec3, 6>> & cutouts,
	            const std::array<glm::mat4, 2> & cutout_mvp,
	            mask_stage_cpu * cpu_stats = nullptr);

	bool has_mesh() const
	{
		return not meshes.empty();
	}

	// Drop cached framebuffers/views (swapchain recreated: old image
	// handles are dead and driver handle recycling could otherwise alias
	// a stale framebuffer onto a new image).
	void reset_targets()
	{
		targets.clear();
	}

private:
	vk::raii::Device & device;
	vk::raii::PhysicalDevice & physical_device;
	vk::Format format;

	std::pair<vk::raii::Buffer, vk::raii::DeviceMemory> make_buffer(vk::DeviceSize size,
	                                                                vk::BufferUsageFlags usage,
	                                                                vk::MemoryPropertyFlags properties);

	vk::raii::RenderPass renderpass{nullptr};
	vk::raii::PipelineLayout pipeline_layout{nullptr};
	vk::raii::Pipeline pipeline{nullptr};

	// LOAD variant of the raster pass for the marker window cutout: same
	// single-attachment shape (framebuffer-compatible with the above), so
	// the finished blurred image can be re-begun and stamped with a crisp
	// full-alpha quad no blur pass touches.
	vk::raii::RenderPass cutout_renderpass{nullptr};

	// SDF seed + Jump Flood workspaces (own RG32F images: full usage
	// control, unlike swapchain images). Sized to this group's SDF divisor
	// (extent/div); the divisor is static per group (keyed by feather-px),
	// so a config change lands on a fresh renderer. Recreated when the
	// mask extent changes.
	struct sdf_target
	{
		vk::raii::Image image{nullptr};
		vk::raii::DeviceMemory memory{nullptr};
		std::vector<vk::raii::ImageView> views;
		std::vector<vk::raii::Framebuffer> fbs;
	};
	// Seed render pass: CLEARs to INF, stores GENERAL (read back by the
	// composite for the inside/outside sign).
	vk::raii::RenderPass seed_renderpass{nullptr};
	vk::raii::Pipeline seed_pipeline{nullptr}; // mask.vert + seed.frag
	// Flood render pass: DONT_CARE load (every texel rewritten), GENERAL.
	vk::raii::RenderPass sdf_renderpass{nullptr};
	vk::raii::Pipeline jfa_pipeline{nullptr}; // blur.vert + jfa.frag
	vk::raii::Pipeline composite_pipeline{nullptr}; // blur.vert + sdf_composite.frag
	vk::raii::PipelineLayout jfa_layout{nullptr};
	vk::raii::PipelineLayout composite_layout{nullptr};
	vk::raii::DescriptorSetLayout jfa_set_layout{nullptr};
	vk::raii::DescriptorSetLayout composite_set_layout{nullptr};
	vk::raii::DescriptorPool descriptor_pool{nullptr};
	// One set per (iteration, eye): descriptor updates are host-side
	// writes completing before submit, so reusing one set across flood
	// iterations would make every draw read the LAST update (a destroyed
	// cascade that composites as a hard edge). Distinct sets keep each
	// binding stable: jfa_sets[eye*8+pass] sources iteration pass,
	// composite_sets[eye] bind (field, seed). Vectors (not arrays): raii
	// handles have no default constructor. Worst case is 7 iterations
	// (128px at div4), so 8/eye has margin.
	static constexpr uint32_t max_flood_passes = 8;
	std::vector<vk::raii::DescriptorSet> jfa_sets;
	std::vector<vk::raii::DescriptorSet> composite_sets;
	vk::raii::Sampler sampler{nullptr};         // linear: flood field reads
	vk::raii::Sampler nearest_sampler{nullptr}; // nearest: exact seed coords
	sdf_target sdf_seed, sdf_ping, sdf_pong;
	// Coverage-sign texture: the silhouette rasterized flat-white at half
	// mask resolution (swapchain format, so the existing raster renderpass
	// and pipeline serve it with an opacity-1 push). The composite reads
	// it linearly for a 2px-grid soft sign edge, decoupling boundary
	// quality from the (coarser) flood resolution.
	sdf_target sdf_cover;
	vk::Extent2D targets_extent{0, 0};
	int targets_div = 0;
	// SDF storage format, chosen once in the constructor: RG16F where the
	// device renders/filters it (halves flood bandwidth; coords < 2048 are
	// exact in half), else RG32F. The seed INF and composite inside/outside
	// threshold follow the choice (sdf_outside / inside_thresh push).
	vk::Format sdf_format_used = vk::Format::eR32G32Sfloat;
	float sdf_outside = 1e10f;
	void ensure_targets(vk::Extent2D extent, int sdf_div);
	void update_flood(uint32_t eye, uint32_t pass, vk::ImageView view);
	void update_composite(uint32_t eye, vk::ImageView field, vk::ImageView cover);

	// One uploaded mesh per object in the group. Staging is per mesh and
	// shared-read (uploads are rare, map-change only); the pending copy
	// applies at the start of the next record().
	struct mesh_buffers
	{
		vk::raii::Buffer vertex_buffer{nullptr};
		vk::raii::DeviceMemory vertex_memory{nullptr};
		vk::raii::Buffer index_buffer{nullptr};
		vk::raii::DeviceMemory index_memory{nullptr};
		uint32_t index_count = 0;
		vk::raii::Buffer staging_buffer{nullptr};
		vk::raii::DeviceMemory staging_memory{nullptr};
		vk::DeviceSize staging_size = 0;
		std::optional<std::pair<vk::DeviceSize, vk::DeviceSize>> pending_upload;
	};
	std::map<std::string, mesh_buffers> meshes;
	void flush_upload(vk::raii::CommandBuffer & cmd, mesh_buffers & mesh);

	// Marker window-cutout quad (two world-space triangles, rewritten per
	// record via the persistent VMA mapping). Created lazily on first
	// cutout use.
	buffer_allocation cutout_verts;

	// Framebuffers + views per swapchain image (images cycle; entries for
	// retired swapchains are dropped via reset_targets()).
	struct frame_targets
	{
		vk::Extent2D extent;
		std::array<vk::raii::ImageView, 2> views;
		std::array<vk::raii::Framebuffer, 2> framebuffers;
		frame_targets(vk::Extent2D extent_,
		              std::array<vk::raii::ImageView, 2> views_,
		              std::array<vk::raii::Framebuffer, 2> framebuffers_) :
		        extent(extent_),
		        views(std::move(views_)),
		        framebuffers(std::move(framebuffers_))
		{
		}
	};
	std::unordered_map<VkImage, frame_targets> targets;
};
