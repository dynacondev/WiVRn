# Roadmap: Quest 3D-Mesh Passthrough + Fiducial Alignment (Native, Option A)

Goal: on Meta Quest, render a server-provided glTF/GLB model as a
surface-projected passthrough cutout (passthrough visible only where the mesh
projects, correct from all angles, composited on-headset), anchored by a
single AprilTag 36h11 fiducial with one-shot alignment.

Locked decisions:

- Native WiVRn client implementation (no Unity host). Single `XrSession` owner
  stays `client/application.cpp`.
- Model format: glTF/GLB only. Reuses `client/render/scene_loader.cpp`. No STL
  loader.
- Server authoritative mapping: `/etc/wivrn/config.json` (plus existing
  `$XDG_CONFIG_HOME`, `/usr/share` overlay chain, see `docs/configuration.md`)
  extended with a fiducial list; models pushed over the WiVRn protocol on
  connect with hash cache. No HTTP, no manual sync.
- Marker: single active AprilTag 36h11 via `XR_EXT_spatial_marker_tracking`
  (+ `XR_EXT_spatial_entity`, `XR_EXT_future`). Size in meters comes from
  config.
- Calibration: one-shot + Quest SLAM hold. No freeze on marker loss, no
  continuous follow. Manual realign button in Stream in-VR GUI with in-view
  status. Mesh pose rule: `meshClientPose = observedMarkerPose *
  markerToMeshOffset`.
- Composition: client-side only. Projected passthrough layer is an overlay on
  top of / cut out from everything else (server video included). This avoids
  server-lag edge jitter by construction: geometry transform + compositing
  happen on-headset at vsync.

Current baseline (why this is new):

- `client/xr/passthrough.h/cpp` + `client/xr/session.cpp` only create a
  `RECONSTRUCTION_FB` layer. No `XR_FB_triangle_mesh`, no `PROJECTED_FB`.
- `client/scene.h` / `client/render/scene_renderer` rasterize glTF into a
  normal projection layer. Projected passthrough never rasterizes in-app; the
  runtime projects camera imagery onto supplied triangle geometry.
- `client/scenes/stream_tracking.cpp` locates against `height_offset_space`
  (Y-only offset on `STAGE`). No full-pose world offset, no marker code.
- Protocol: `common/wivrn_packets.h` (`from_headset::packets`,
  `to_headset::packets`, hashed via `common/protocol_version.h`) has no
  fiducial/model messages.

## Phase 1 — Server config + model transport + client cache

Scope: define the mapping and get bytes to the headset. No rendering yet.

1. Server config (`docs/configuration.md`, server config loader):
   `fiducial_map: [{marker_id, marker_size_m, model_path, position[3],
   orientation[4 xyzw], scale[3] or float}]`. Pose + scale only, per prior
   agreement. `marker_id` is the AprilTag 36h11 ID. Paths resolved
   server-side, blobs read at session start.
2. Protocol (`common/wivrn_packets.h`, `common/protocol_version.h`):
   new `to_headset::fiducial_map{entries}` + `to_headset::model_blob{hash,
   bytes}` (glB). Bump/check `protocol_revision` handling so mismatched
   client/server fails cleanly. Chunking must respect existing
   `video_stream_data_shard::max_payload_size`-style TCP framing; prefer the
   existing reliable channel used for `application_list`/`application_icon`,
   not the video shard path.
3. Client cache: store by content hash in app storage, re-send only on change.
   On connect: compare server hash list vs local, request missing blobs.
4. Validation without rendering: client logs `fiducial entries N, model hash
   H, bytes B, glTF parse OK`, plus a debug GUI line. No `XrTriangleMesh`
   yet.

Accept: connect with 1-entry config pushes a 1–10MB glB once, second connect
sends hashes only; corrupt/oversize blob surfaces as `server_message error`.

## Phase 2 — Projected passthrough mesh (MVP)

Scope: testable MVP. Given a model from Phase 1 (or a bundled fallback for
bring-up), show it as a passthrough cutout overlay. No marker required yet:
place at config pose (or 1m in front as debug fallback) so transport +
compositing can be tested standalone.

1. Instance extensions (`client/application.cpp:initialize` opt list):
   add `XR_FB_triangle_mesh_EXTENSION_NAME`. Keep Quest-gated via
   `client/hmd_traits` (`manufacturer == Oculus`) so other headsets are
   unaffected. `XR_FB_passthrough` already opted in.
2. Passthrough layer (`client/xr/passthrough.h/cpp`,
   `client/xr/session.cpp`): add second layer with
   `XrPassthroughLayerCreateInfoFB{purpose =
   XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB}`. Wrappers for
   `xrCreateTriangleMeshFB`, `xrTriangleMeshBeginUpdateFB` /
   `xrTriangleMeshEndUpdateFB`, `xrCreateGeometryInstanceFB` /
   `xrDestroyTriangleMeshFB`, per-frame
   `XrPassthroughGeometryInstanceCreateInfoFB{mesh, baseSpace, pose, scale}`
   transform update. Keep existing reconstruction path untouched.
3. glTF flatten: walk the loaded `entt::registry` (`renderer::mesh /
   primitive`, `components::node` in `client/render/scene_components.h`),
   bake node transforms, emit a single indexed triangle list (positions only
   for the runtime mesh). Handle glTF Y-up, unit meters, winding/CCW vs
   `front_face`, and multi-primitive scenes. Normals/UVs/materials are
   irrelevant to the runtime mesh.
4. Submit (`client/scene.h`, `client/scenes/stream.cpp:render`,
   `client/scenes/lobby.cpp` if debug): `render_start` path must submit
   `XrCompositionLayerPassthroughFB` (projected, overlay) after the video
   projection layer, `environmentBlendMode = OPAQUE` per Meta requirement.
   Rest of scene stays as-is (opaque video + local overlays).
5. Debug placement: config pose first, else fixed offset in `WORLD`/`STAGE`
   space. Transform update runs locally every frame from headset space, never
   from server timestamps.

MVP test (must pass before Phase 3):

- Quest 3, WiVRn server with 1-entry `fiducial_map` + small glB (<10MB).
- Connect, Stream GUI shows model loaded (hash/bytes).
- Look around: passthrough visible only through the mesh silhouette, stable
  from multiple angles, occluding server video (overlay). No app-shaded mesh
  double-render.
- Disconnect/reconnect: no re-download (hash cache hit).
- Non-Quest headset or missing extension: clean fallback, no crash.

Known risks: `XR_FB_triangle_mesh` vertex/index limits and winding; layer
count limits (`maxLayerCount`); Unity `AddSurfaceGeometry` deprecation noise
does not apply to raw OpenXR but verify against installed Meta OpenXR SDK
headers and Horizon OS version on test device.

## Phase 3 — AprilTag 36h11 tracking + in-view status

Scope: detect the marker, report pose + status. No alignment writes yet.

1. Extensions + wrapper (`client/application.cpp`, new
   `client/xr/marker_tracker.h/cpp`): enable `XR_EXT_spatial_entity`,
   `XR_EXT_future`, `XR_EXT_spatial_marker_tracking`. Async flow:
   `xrCreateSpatialContextAsyncEXT` with
   `XrSpatialCapabilityConfigurationAprilTagEXT{aprilDict =
   36h11, size from config}` -> poll future -> `xrDiscoverSpatialEntities` ->
   per-entity pose component locate vs client `WORLD`. Single-marker only:
   filter by `markerId == config.marker_id`.
2. Integrate into `client/scenes/stream_tracking.cpp:locate_spaces_functor`
   (same cadence as head/controller locates, same `predicted_display_time`).
   Expose `markerTracked bool + lastPose + timestamp` to scene state.
3. Stream GUI (`client/scenes/stream_gui.cpp`, `stream_actions.cpp`):
   in-view dot + `markerId/size` + age of last sighting. No button yet.

Accept: point at printed 36h11 tag of configured size/ID -> dot green + pose
updating; cover tag -> dot red, last pose retained but flagged stale.

Risks: Horizon OS version skew (known v206 spatial-marker regression report);
capability absent on older runtimes -> must degrade to Phase-2 static
placement with a clear status line.

## Phase 4 — One-shot calibrate + world alignment

Scope: wire the button. `meshClientPose = observedMarkerPose *
markerToMeshOffset`, plus one-shot world recenter for server video.

1. Offset math (client, `stream_tracking.cpp` + new alignment helper):
   `markerToMeshOffset` built from config `pos/quat/scale` (mesh relative to
   marker). On button press, if `markerTracked`: set projected geometry
   instance base pose to `observed * offset`. Store as the persistent mesh
   anchor; thereafter update its reported transform from SLAM (same space),
   ignoring marker loss.
2. World recenter: generalize `height_offset_space` (today `{0,-h,0}` in
   `stream_tracking.cpp`, `configuration::get_height_offset`) to a full
   `XrPosef` origin offset applied to `STAGE`-derived locates before
   `from_headset::tracking` send (`server/driver/wivrn_session.cpp` already
   handles `recentered` via `xrt_space_overseer_recenter_local_spaces`; keep
   that path). Compute yaw+position correction only (preserve height offset
   semantics), from the same single observation used for the mesh. Manual
   button re-runs it; no auto re-arm on loss.
3. UI: Stream GUI `Calibrate / Realign` button (enabled only when tracked),
   plus last-alignment error/age readout. Persist mesh anchor + world offset
   for the session; clear on disconnect.

Accept: with tag in view press Calibrate -> mesh snaps to tag-relative pose
and stays world-locked while walking around; cover tag -> mesh stays (SLAM);
move tag, press again -> re-snaps. Server video content shifts consistently
with the one-shot recenter (no per-frame swimming).

Non-goals in this phase: multi-marker averaging, persistent-across-sessions
anchors, exposing the marker as a SteamVR tracked device on the server.

## Phase 5 — Hardening + docs

- Quest-only gating + capability checks with user-readable fallback strings.
- `AndroidManifest.xml` review (passthrough already declared; confirm no new
  permission needed for the OpenXR marker path vs Camera API).
- Limits: model size cap + error toasts (`to_headset::server_message`),
  oversized glB rejected before upload.
- Docs: `docs/configuration.md` schema + example JSON, Stream GUI strings,
  Horizon OS / Quest 3 tested version noted.
- Device matrix: Quest 3 primary; Quest Pro/3S best-effort; Pico/HTC must not
  regress (extension absent -> old behavior).

Example config fragment (Phase 1):

```json
{
	"fiducial_map": [
		{
			"marker_id": 42,
			"marker_size_m": 0.08,
			"model_path": "/usr/share/wivrn/meshes/widget.glb",
			"position": [0, 0.05, 0.1],
			"orientation": [0, 0, 0, 1],
			"scale": 1.0
		}
	]
}
```

`position/orientation/scale` is the marker-to-mesh offset applied per
`meshClientPose = observedMarkerPose * markerToMeshOffset`.
