# Roadmap: Quest 3D-Mesh Passthrough + Fiducial Alignment (Native, Option A)

Goal: on Meta Quest, render a server-provided glTF/GLB model as a
surface-projected passthrough cutout (passthrough visible only where the mesh
projects, correct from all angles, composited on-headset), anchored by a
single QR-code fiducial with one-shot alignment.

Locked decisions:

- Native WiVRn client implementation (no Unity host). Single `XrSession` owner
  stays `client/application.cpp`.
- Model format: glTF/GLB only. Reuses `client/render/scene_loader.cpp`. No STL
  loader.
- Server authoritative mapping: `/etc/wivrn/config.json` (plus existing
  `$XDG_CONFIG_HOME`, `/usr/share` overlay chain, see `docs/configuration.md`)
  extended with a fiducial list; models pushed over the WiVRn protocol on
  connect with hash cache. No HTTP, no manual sync.
- Marker: single active QR code via `XR_EXT_spatial_marker_tracking`
  (+ `XR_EXT_spatial_entity`, `XR_EXT_future`), matched by exact payload
  string. Size in meters comes from config.
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
   `fiducials: [{id, markers: [{marker_data, marker_size_m, position[3],
   orientation[3] degrees}]}]` plus `passthrough: [{type, id, fiducial: [ids],
   model_path, position[3], orientation[3] degrees, scale, feather-px,
   fade-in-ms}]`. `marker_data` is the exact QR payload string. Paths
   resolved server-side, blobs read at session start.
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

   (Superseded after Phase 4: the projected mesh now appears only after the
   user presses Calibrate, anchored at observedMarkerPose * configOffset.
   Pre-calibration behavior is stock upstream, which also gives a clean
   baseline for diagnosing video issues.)

MVP test (must pass before Phase 3):

- Quest 3, WiVRn server with 1-fiducial + 1-object config + small glB (<10MB).
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

## Phase 3 — QR-code tracking + in-view status

Scope: detect the marker, report pose + status. No alignment writes yet.

1. Extensions + wrapper (`client/application.cpp`, new
   `client/xr/marker_tracker.h/cpp`): enable `XR_EXT_spatial_entity`,
   `XR_EXT_future`, `XR_EXT_spatial_marker_tracking`. Async flow:
   `xrCreateSpatialContextAsyncEXT` with
   `XrSpatialCapabilityConfigurationQrCodeEXT{size from config}` -> poll
   future -> discovery snapshots -> per-entity MARKER + BOUNDED_3D locate vs
   client world space. The runtime reports markerId 0 for QR, so filter by
   exact decoded payload (`marker-data`). Single-marker only.
2. Integrate into `client/scenes/stream_tracking.cpp:locate_spaces_functor`
   (same cadence as head/controller locates, same `predicted_display_time`).
   Expose `markerTracked bool + lastPose + timestamp` to scene state.
3. Stream GUI (`client/scenes/stream_gui.cpp`, `stream_actions.cpp`):
   in-view dot + `markerId/size` + age of last sighting. No button yet.

Accept: point at printed QR code of configured size/payload -> dot green +
pose updating; cover tag -> dot red, last pose retained but flagged stale.

Risks: Horizon OS version skew (known v206 spatial-marker regression report);
capability absent on older runtimes -> must degrade to Phase-2 static
placement with a clear status line.

## Phase 4 — One-shot calibrate (object placement only)

Scope: wire the button. `meshClientPose = observedMarkerPose *
markerToMeshOffset`. Deliberately NO world-origin change: shifting the
client origin moves it out from under the server-rendered video (the game
is rendered against the session-start origin), displacing the video quad.
The mesh is placed purely as an object in the stable SLAM frame, which the
headset holds drift-free. (A marker-as-origin recenter was tried and
reverted for exactly this reason.)

1. Offset math (client): `markerToMeshOffset` built from config
   `pos/quat/scale` (mesh relative to marker). On button press, if
   `markerTracked`: set the mesh anchor to `observed * offset`. Store as
   the persistent mesh anchor; thereafter hold it in the same space,
   ignoring marker loss.
2. UI: Stream GUI `Calibrate / Realign` button (enabled only when tracked),
   plus last-alignment error/age readout. Persist mesh anchor for the
   session; clear on disconnect.
3. Tracking origin stays height-only (`{0,-h,0}` in `stream_tracking.cpp`):
   no yaw/XZ from calibration, ever.

Accept: with tag in view press Calibrate -> mesh snaps to tag-relative pose
and stays world-locked while walking around; cover tag -> mesh stays (SLAM);
move tag, press again -> re-snaps. Server video never shifts (no recenter).

Non-goals in this phase: multi-marker averaging, persistent-across-sessions
anchors, exposing the marker as a SteamVR tracked device on the server.

## Phase 6 (future) — Server-side virtual-world alignment

Goal: "the simulation renders from where the player virtually moved,"
without touching the client origin. The existing
`from_headset::tracking::state_flags::recentered` channel means something
else ("recenter local spaces to the current physical pose") and must not
be reused for this.

Sketch: new explicit-offset control packet (yaw + dx + dz, client to
server, sent on Calibrate alongside the local mesh placement); the server
composes it into its tracking origin so subsequently rendered game views
already include the shift, keeping video layers and mesh consistent by
construction. Open UX questions: who initiates (headset button vs
dashboard), persistence across sessions, interaction with the existing
recenter flag and SteamVR lighthouse origin. Design jointly with the
game-side need; no client origin shift, ever.

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
	"fiducials": [
		{
			"id": "press",
			"markers": [{"marker-data": "wivrn:11", "marker-size-m": 0.08}]
		}
	],
	"passthrough": [
		{
			"type": "3d-passthrough",
			"id": "press-window",
			"fiducial": ["press"],
			"model-path": "/usr/share/wivrn/meshes/widget.glb",
			"position": [0, 0.05, 0.1],
			"orientation": [0, 0, 0],
			"scale": 1.0
		}
	]
}
```

`position/orientation/scale` compose per
`fiducialPose = observedMarkerPose * markerToFiducialOffset` then
`objectPose = fiducialPose * fiducialToObjectOffset` (scale last).

---

# Roadmap: macOS Server v1 MVP (bespoke minimal OpenXR runtime)

Goal: `wivrn-server-macos` executable on Apple Silicon macOS that runs
macOS-native OpenXR apps (proof: `hello_xr` cube) and streams to the
unmodified Android client over the existing WiVRn wire protocol.

Locked decisions (2026-10-13):

- Bespoke clean-room runtime in `server/macos-runtime/` (NOT a Monado
  fork, NOT VDXR/OXRSys/DisplayXR/MetalXR/ox code). Pattern borrowed
  from VDXR/OXRSys frame-flow discipline only.
- x264-only encode; no audio; no D-Bus in v1; Bonjour (`dns_sd.h`)
  discovery; hermetic deps via CMake FetchContent + mise-pinned LunarG
  SDK; MoltenVK version gate relaxed (warn, don't fatal).
- Apple Silicon only; stereo-only; Vulkan-only binding (MoltenVK);
  OPAQUE-only; single instance/system/session; simple-controller input
  only (no hands/eye/face/body/gamepad-mirroring/haptics-hardware).
- Isolation: Linux `server/`, `patches/monado/`, `monado-rev` untouched
  except `if(NOT APPLE)` CMake guards. Reuse `common/` by link with
  thin adapters in the new folder (never refactor Linux files).
- Wire compat sacred: `common/wivrn_packets.h` bit-identical, 9757
  TCP+UDP + `_wivrn._tcp` TXT keys, so Android client + Linux server
  stay in sync.

## Phase 0 — Scaffolding [status: in progress]

- [ ] `server/macos-runtime/` + `CMakeLists.txt` + `wivrn-server-macos`
  target (own `main.cpp`, no `ipc_server_main_common`).
- [ ] Preset `macos-server`: `WIVRN_BUILD_SERVER=OFF`,
  `WIVRN_BUILD_MACOS_SERVER=ON`, `NVENC/VAAPI/VULKAN_ENCODE/PIPEWIRE/
  WIVRNCTL/LH/SOLARXR=OFF`, `X264=ON`.
- [ ] Top-level CMake `NOT APPLE` guards (AVAHI/glib-unix/libnotify/
  LIBDRM/pipewire/systemd, systemd/firewalld installs) + MoltenVK
  gate relax.
- [ ] `mise.toml`: LunarG SDK pin + `PKG_CONFIG_PATH` docs.

Accept: `cmake --preset macos-server` configures on macOS arm64.

## Phase 1 — common/ portability shims

- [ ] `wivrn_sockets.cpp`: `recvmmsg/sendmmsg` → `recvmsg` loop,
  `MSG_NOSIGNAL` → `SO_NOSIGPIPE`, `MSG_DONTWAIT` → `O_NONBLOCK`,
  `<netinet/in.h>`.
- [ ] `named_thread.h`: `__APPLE__` → `pthread_setname_np(name)` branch.
- [ ] Homebrew OpenSSL 3 (`OPENSSL_ROOT_DIR`); Boost/Eigen/CLI11
  hermetic-or-brew.

Accept: `wivrn-common` builds on macOS arm64.

## Phase 2 — Runtime skeleton

- [ ] `loader_entry.cpp` (`xrNegotiateLoaderRuntimeInterface` +
  dispatch + `wivrn-macos.json`), `runtime.*` (singleton
  instance/system), `session.cpp` (single-session state machine),
  `views.cpp` (stereo/OPAQUE, extents from headset `info` packet).

Accept: loader resolves via `XR_RUNTIME_JSON`, `xrCreateInstance/
GetSystem` succeed.

## Phase 3 — Poses + simple-controller input

- [ ] `spaces.cpp` (`LOCAL/LOCAL_FLOOR/STAGE`, `LocateViews/Space`
  from `from_headset::tracking`).
- [ ] `actions.cpp` (simple-controller + Vive/Index→touch aliases,
  `SyncActions/GetActionState*`; haptics forward or no-op).
- [ ] `wivrn_connection` adapter (drop `wivrn_ipc_socket_monado`
  pollfd); `configuration.cpp` JSON merge reuse.

Accept: headset tracking drives view poses; controller buttons/sticks
visible to app.

## Phase 4 — Vulkan swapchains + frame loop

- [ ] Trimmed `vk_bundle` subset (drop `*_FD/VIDEO/DMA_BUF/
  encode_queues/share()`, single MoltenVK queue + own mutex).
- [ ] Vulkan-only binding; `Acquire/Wait/Release` over app `VkImage`s.
- [ ] Port `pacer` math, `foveation` (`Xr*` types, same `.comp`),
  new minimal squasher over `XrCompositionLayerProjection` (+`Quad`
  if cheap).

Accept: app renders stereo frames into runtime swapchains at display
rate without validation-layer errors.

## Phase 5 — Encode + network + discovery

- [ ] `video_encoder_x264` + `raw` via `encode(info, sink)` adapter;
  host-visible copy verified modifier-free on MoltenVK.
- [ ] `accept_connection` adapter (9757), `send_shard` sink, IDR
  handler unchanged.
- [ ] `bonjour_publisher.cpp` (`dns_sd.h`, same TXT keys).

Accept: `xrEndFrame` → squash → x264 → 9757 → client → poses back →
`xrWaitFrame` prediction loop closes.

## Phase 6 — Bring-up + proof

- [ ] `hello_xr` (Vulkan/MoltenVK) via `XR_RUNTIME_JSON` streams live
  cube to headset.

Accept: live cube, pose-tracked views. No real-game compat, no CTS.

## Phase 7 — Executable MVP packaging

- [ ] CLI + config (`~/Library/Application Support/wivrn/` candidate,
  XDG kept); `launchd` plist sample. No `.app`, no
  signing/notarization.

Accept: `./wivrn-server-macos` runs headless, reconnects cleanly.

## v2 backlog (explicitly deferred, do not build in v1)

- VideoToolbox HW encode (H.264/HEVC/Main10 + Rosetta-helper
  question) + BT.709 color contract + ABR/FEC hardening.
- CoreAudio input/output.
- D-Bus control plane (keep `io.github.wivrn.Server.xml` names;
  decide brew `dbus-daemon` vs new transport) + `wivrnctl` port.
- Gamepad/HID mirroring (`CGEvent`/virtual-HID + entitlements) +
  full haptics.
- Hands/eye/face/body tracking.
- Dashboard (Qt/Kirigami bundling; `avahi/firewall/steam` →
  Bonjour/`pf`/Steam.app rewrites).
- `.app` bundle + signing/notarization + Homebrew formula.
- Intel/universal builds.
- Real-game compat + CTS lane.
- `xrizer`/OpenComposite glue for OpenVR-API games.
