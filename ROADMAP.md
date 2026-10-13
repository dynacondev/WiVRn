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

## Phase 0 — Scaffolding [status: done 2026-10-13]

- [x] `server/macos-runtime/` + `CMakeLists.txt` + `wivrn-server-macos`
  target (own `main.cpp`, no `ipc_server_main_common`).
- [x] Preset `macos-server`: `WIVRN_BUILD_SERVER=OFF`,
  `WIVRN_BUILD_MACOS_SERVER=ON`, `NVENC/VAAPI/VULKAN_ENCODE/PIPEWIRE/
  WIVRNCTL/LH/SOLARXR=OFF`, `X264=ON`.
- [x] Top-level CMake: `WIVRN_BUILD_MACOS_SERVER` option, `NEED_COMMON`
  coverage, MoltenVK gate warn-only on APPLE. (`NOT APPLE` guards on
  the AVAHI/glib-unix/libnotify/LIBDRM/pipewire/systemd blocks
  deferred: the macos-server preset keeps `WIVRN_BUILD_SERVER=OFF`
  so they don't trigger; revisit if a unified preset ever enables
  both.)
- [x] `mise.toml`: LunarG SDK pin + `PKG_CONFIG_PATH` docs.

Accept: `cmake --preset macos-server` configures on macOS arm64.

Verified: configures (Vulkan 1.4.363, Boost 1.92, OpenSSL 4.0.3 via
brew), `wivrn-server-macos` scaffold builds + runs exit 0. Note:
full LunarG SDK cannot be mise-pinned (no aqua package, installer-only
upstream, brew has no pins) — hermetic path is CMake FetchContent
(Vulkan-Headers + Vulkan-Loader + MoltenVK); `dns_sd.h` confirmed
system-provided (libSystem, no dep).

## Phase 1 — common/ portability shims [status: done 2026-10-13]

- [x] `wivrn_sockets.cpp`: `recvmmsg/sendmmsg` → `recvmsg` loop,
  `MSG_NOSIGNAL` → `SO_NOSIGPIPE`, `MSG_DONTWAIT` → `O_NONBLOCK`,
  `<netinet/in.h>`.
- [x] `named_thread.h`: `__APPLE__` → `pthread_setname_np(name)` branch.
- [x] Homebrew OpenSSL 3 (`OPENSSL_ROOT_DIR`); Boost/Eigen/CLI11
  hermetic-or-brew.

Accept: `wivrn-common` builds on macOS arm64.

Verified: `libwivrn-common.a` builds; loopback smoke test passes
(TCP `send_raw`+`send_many_raw`, UDP `send_raw`+`send_many_raw`+
`receive_raw` via the Darwin fallbacks). Incidental fixes found by
the macOS build, all Linux-neutral: `IPV6_JOIN/GROUP` aliases,
`msg_iovlen` `static_cast` (Linux `size_t` vs Darwin `int`),
`sprintf`→`snprintf` in `hexdump`, preset `CMAKE_CXX_FLAGS=
-Wno-error=undef` (LunarG `vulkan.hpp` `#if (VULKAN_HPP_ASSERT ==
assert)` trips Clang `-Wundef`; per AGENTS.md host-config advice).

## Phase 2 — Runtime skeleton [status: done 2026-10-13]

- [x] `loader_entry.cpp` (`xrNegotiateLoaderRuntimeInterface` +
  dispatch + `wivrn-macos.json`), `runtime.*` (singleton
  instance/system), `session.cpp` (single-session state machine),
  `views.cpp` (stereo/opaque, extents from headset `info` packet).

Accept: loader resolves via `XR_RUNTIME_JSON`, `xrCreateInstance/
GetSystem` succeed.

Verified: `wivrn-macos-runtime.dylib` + hermetic FetchContent loader
(`release-1.1.59`, same as client) + `wivrn-macos-runtime-test`
PASSes end to end through the real loader: negotiate, instance
(good/bad-ext/bad-version), props, HMD system, stereo views
(2x2064x2208), OPAQUE blend, session IDLE<->SYNCHRONIZED lifecycle,
Vulkan-bound session refused (Phase 4), singleton
create-destroy-create. Findings: `xrCreateInstance` must be
NULL-instance retrievable (loader jumps to 0 otherwise); the loader
pre-filters unknown extensions/versions and rejects simultaneous
instances, so negative CreateInstance checks run solo. Runtime links
headers only, never the loader lib.

## Phase 3 — Poses + simple-controller input [status: done 2026-10-13]

- [x] `spaces.cpp` (`LOCAL`/`LOCAL_FLOOR`/`STAGE`/`VIEW`, `LocateViews/
  Space` from `from_headset::tracking`).
- [x] `actions.cpp` (simple-controller + Vive/Index→touch aliases,
  `SyncActions/GetActionState*`; haptics forward or no-op).
- [x] `wivrn_connection` adapter (drop `wivrn_ipc_socket_monado`
  pollfd); `configuration.cpp` JSON merge reuse.

Accept: headset tracking drives view poses; controller buttons/sticks
visible to app.

Verified: injectable `TrackingSource` (`tracking.h/cpp`) with a
deterministic synthetic source (HMD at y=1.6 ±10° yaw/8 s, static
controllers, right-hand pressed demo state); `wivrn-macos-runtime-test`
covers path atoms roundtrip, action sets/actions, batched bindings +
vive `trigger/click` alias, attach (double-attach refused), sync,
boolean/float/vector2/pose states with both polarities, bound
sources, reference + grip action spaces (grip locates at exactly
(0.2, -0.3, -0.3) in LOCAL), stereo `LocateViews` with ±32 mm eye
split — PASS. Findings: component matching must use the `/input/`
suffix (paths carry the `/user/hand/...` prefix); `suggest` is
most-recent-wins per profile so tests batch one call per profile;
`receive_pending()` must be drained before `receive_raw()` (pre-existing
upstream reassembly behavior). `configuration.cpp` merge deferred to
Phase 5 (needed with networking, not before).

## Phase 4 — Vulkan swapchains + frame loop [status: done 2026-10-13]

- [x] Trimmed `vk_bundle` subset (drop `*_FD/VIDEO`, single MoltenVK
  queue + own mutex).
- [x] Vulkan-only binding; `Acquire/Wait/Release` over app `VkImage`s.
- [x] Port `pacer` math, `foveation` (`Xr*` types, same `.comp`),
  new minimal squasher over `XrCompositionLayerProjection` (+`Quad`
  if cheap).

Accept: app renders stereo frames into runtime swapchains at display
rate without validation-layer errors.

Verified: `vk_context` (runtime-owned triple-buffered images,
device-local memory, real per-image fences), `graphics` (MoltenVK
requirements 1.0–1.3, instance/device extension lists, enable2
create-functions), `swapchain` (formats/acquire/FIFO wait/release
with GPU-signaling empty submits), `frame` (Wait/Begin/End with
call-order tracking, fixed-period phase-locked prediction, projection
intake validation), `wivrn-macos-graphics-test` headless MoltenVK
drive (Apple M3 Max): DIY + runtime-created devices, 2x512x512
triple-buffered swapchains, 5-frame loop with advancing predictions,
TIMEOUT honesty, quad refusal — PASS with **0 validation errors**.
Rescope (recorded, not dropped): foveation compute + squasher GPU
submit move to Phase 5 with encode (they are only exercisable against
real pixels); Phase 4 keeps the layer-intake validation seam
(`lastLayerCount`) + fixed-period pacer (presentation feedback in
Phase 5).
Findings: 1.1 `XrGraphicsBindingVulkan2KHR` is a struct alias of the
v1 struct (family + index, no queue handle — runtime resolves the
queue itself); the `...Extensions2KHR` string queries do not exist
(v1 5-arg names + create-functions instead); `XrResult` timeout is
`XR_TIMEOUT_EXPIRED` (not `XR_ERROR_*`); Vk structs use `pNext`;
`VkSubmitInfo` release-signaling is only legal on waited (reset)
fences; wait carries no index so v1 uses oldest-outstanding FIFO;
re-wait must really re-wait (no success short-circuit) to keep
TIMEOUT honest; `VkInstance`/`VkDevice` want api 1.1 (makes
`get_physical_device_properties2` core, satisfying the
portability_subset VUID-01387 — the ext is not offered at device
level); both `enable` + `enable2` advertised (DIY + runtime-created
devices); runtime links libvulkan directly (loader-less dispatch a
la OXRSys). Env note: duplicate `MVKBlockObserver` (brew + LunarG
MoltenVK both load) warns but is benign; pin `VK_ICD_FILENAMES` if it
ever bites.

## Phase 5 — Encode + network + discovery [status: in progress]

- [x] Slice A (done 2026-10-13): `snapshot` (COLOR_ATTACHMENT_OPTIMAL
  → TRANSFER_SRC → host BGRA, restored) + `X264Encoder` (ultrafast/
  zerolatency/main/ABR/Annex-B, CPU BT.709 full-range BGRA→NV12 —
  x264 has no RGB→420 main-profile path) + `shard_frame` (≤1400 B,
  IDR/config reliable). `wivrn-macos-encode-test` headless PASS.
- [x] Slice B (done 2026-10-13): `net` (TCP accept + 9-step handshake
  with `encryption_disabled`, UDP stream pinning, control/stream RX
  threads, TX on EndFrame thread) + `NetworkSource` tracking/inputs
  feed + `HeadsetClock` timesync EWMA. `wivrn-macos-net-test`
  loopback fake-headset PASS (handshake, info, tracking→sample,
  +80 ms timesync convergence, NACK→IDR, settings event).
  Findings: `receive_from_raw` MSG_PEEK|null is empty on Darwin
  (dedicated 64 KB heap recvfrom instead); received packets borrow
  socket buffers (validate inline, copy scalars only); shutdown
  ordering must be accept-joins-RX-then-teardown (else UAF/deadlock).
- [x] Slice C+D (done 2026-10-13): `bonjour` (`_wivrn._tcp` + Linux
  TXT keys via dns_sd.h), `streamer` (per-eye snapshot→x264→shard→
  send on EndFrame, view_info with offset-converted display time +
  echoed views, IDR on NACK/settings/first-frame, desc+control on
  connect, timesync every 120 frames, session pacing from headset
  rate, per-session GPU teardown), `wivrn-server-macos` launcher
  (`--port/--runtime -- APP`, XR_RUNTIME_JSON + WIVRN_PORT env).
  `wivrn-macos-stream-test` full loopback PASS: 22/22 eye frames
  (20 stream + 2 reliable IDR), SPS on control after NACK, view_info
  display_time>0 + Annex-B, live HEAD (4,5,6) locates at (4.00, 3.40,
  6.00) in LOCAL. Headless runs opt out via WIVRN_MACOS_HEADLESS
  (else tests would bind/advertise); link starts at CreateSession so
  headsets connect during app startup (documented v1 UX: app runs
  first). Foveation GPU deferred to fast-follow (empty foveation in
  view_info, client handles it); env note: a sandbox permissions
  fault once blackholed post-handshake UDP both ways (handshake/TCP
  unaffected) — cleared externally, suites re-verified green.

Accept: `xrEndFrame` → squash → x264 → 9757 → client → poses back →
`xrWaitFrame` prediction loop closes. **CLOSED on loopback** (real
headset validates timing/quality next).

Headset run 1 (2026-10-13, Quest, hello_xr): handshake + tracking
fine, but the client crashed in `xrCreateSwapchain` — our
`view_info.foveation` went out EMPTY, which decodes to a 0x0
defoveated size on the client. Fixed the same day: send identity
single-segment params (`{w}`/`{h}`, ratio 1) decoding to the full
stream size; loopback test now asserts the decode. Awaiting retest.
Headset run 2 (same day): one light-blue frame then stall with client
`Drop shard for old frame` — EndFrame-thread feed (~30 ms/eye at
2064x2208) blocked the app loop behind slow frames. Fixed with
worker-thread encode: EndFrame only snapshots + stages into a
latest-only mailbox, a worker thread opens/feeds/sends (eye lock
released across the slow feed; `feeding` flag + `dead` checks guard
session teardown mid-feed; worker_loop try/catch so one bad frame
can never silently kill the stream; open-fail resets `feeding`).
Loopback test converted to closed-loop framing (fixed 12-frame burst
was overwritten faster than the worker drained — 1-in-4 flake — now
frames until 4 reassembled or 400 cap): 10/10 PASS (6–26 stream
frames, NACK→IDR every run). Awaiting retest.
Headset run 3 (same day): worker healthy (180+ encodes/eye, both first
frames decoded 2064x2208) but client frozen on first frame 29s, then
control died. Prime suspect: display_time in the wrong clock domain —
offset is 0 until the first timesync response, but the first query went
out only after ~120 encodes (~10s+), so the client holds future-dated
frames. Fixes: timesync burst x4 over reliable control at connect
(first sample snaps the offset), UDP send failure no longer latches
disconnect (drop frame, keep streaming; only control-path failure
drops the link — the latch once froze a live session behind healthy
encode logs), first-sample/first-tracking/NACK/settings/pin-addr logs
+ sent/drop counters in the periodic line as permanent alibis.
Loopback 5/5 PASS. Awaiting retest.
Headset run 5 (same day, fb journey logging): stream FULLY LIVE —
24 frames/eye in 3.6s, every one completed→decoded→displayed with dense
per-eye indices; UDP pinning, tracking, timesync (-5.4d offset snapped
in 9ms) all healthy. The "frozen" look is hello_xr's scene: its cubes
use fixed rotations (openxr_program.cpp:74-83), so consecutive frames
are pixel-identical and client reprojection (live poses) is the only
visible motion. Run 4's "NACK storm" was completion receipts, not
starvation. Real bug found via Linux idr_handler: our NACK gate
(times_displayed==0) fired on every healthy completion receipt and
forced ~100% IDRs (fat TCP frames ~= the 6.7 pairs/s ceiling). Now
mirrors Linux: need_idr iff !sent_to_decoder and frame_index >=
per-stream last_ack (newest received_from_decoder); fb journey logged
on disp==0 only (per-display updates flooded 240/s); worker picks up
the live force_idr flag at take (mailbox overwrites swallowed most
NACK IDRs). Loopback PASS. Awaiting motion test (controller wave).
Headset run 6 (same day): transport perfect (600 sends, drop=0, client
completed to frame 326 dense, zero NACK IDRs — the Linux-faithful gate
holds), but still no hand cubes. Root cause: our tracking_control
requested HEAD-only, so the client only ever sent HEAD (its pattern log
matched ours exactly) — controller samples stayed at origin identity,
and hello_xr's grip cubes rendered out of view. Fix: pattern now asks
HEAD + LEFT/RIGHT_GRIP + LEFT/RIGHT_AIM (11ms); controller sides go
active only while their devices appear in packets (asleep controllers
read inactive, not stale); first-grip receipt logs `grips live`.
Client TEMP-DEBUG logging added (separate revertable commit): tracking
devices+positions every ~3s, per-eye presented-frame changes, genuine
NACKs. Loopback PASS. Awaiting controller-wave retest (controllers
awake: button press first).
Headset run 7 (same day): grips flow end to end (`grips live`, client
grip poses live and moving) but still no cubes. Root cause: coordinate
frames — view_info echoed the client's VIEW-frame views (near-origin)
while grips locate through OUR LOCAL (fixed (0,1.6,0) in the client
room frame), stranding all located content ~room displacement away
(hello_xr cubes at 5m+ off-axis). Linux sends the app's own layer poses
instead. Fix: LOCAL latches to the room frame at connect (floor under
HMD + HMD yaw, Quest-style, re-latched per connection); view_info poses
come from the app's submitted layer views (FOV echo kept — angles are
frame-free); one-shot logs for the latched origin + first sent view
poses. Loopback stream test updated to the latch semantics
(VIEW-in-LOCAL (0,5,0) for fake HEAD (4,5,6)). All suites PASS.
Awaiting retest — static cubes + grip cubes should both be in view.
Headset run 8 (same day): view panel tracks, still no cubes. Forensics
closed it: staged pixels are uniformly (151,151,119) even though the app
records 38k draws into OUR images (api_dump: framebuffers wrap our views,
layouts match, single queue/thread/driver FIFO). Isolated replication in
loopback tests proved (a) LOAD preserves, transfer copies are exact, draws
+ push constants + indexed + culling all behave, but (b) hello_xr's exact
vkCmdClearAttachments writes X instead of slate (fullscreen and sub-rect,
fresh images too), and (c) hello_xr's exact cube draw (its SPV + verts +
BACK+CW) is fully culled while CULL_NONE shows it. So on this MoltenVK,
hello_xr as-is can never display (ClearAttachments-X bg + culled cubes) —
app/driver quirks, not the runtime. New `wivrn-macos-demo` vehicle instead
(animated bg via loadOp-CLEAR + spinning cube, no depth/culling/
ClearAttachments — only verified paths). Awaiting demo headset retest.
Headset run 9 (demo): animated frames stream end to end (26fps/eye @1024,
drop=0, sparse NACKs recovered) — v1 motion proof closed. Image is
HUD-fixed with flat stereo (demo rendered NDC-direct). Fixed with per-eye
view-projection (FOV + runtime pose per eye; cube at LOCAL (0,1,-2)):
world-fixed + real disparity, math verified numerically. Awaiting retest.
Headset run 10: demo showed cycling bg only (pink), hello_xr flat
SlateGrey — both clears working, zero geometry. Root cause (code-read,
not driver): LocateViews reported FOV {+0.86 x4} — equal edges divide
by zero in every projection builder → INF matrices → all draws clip
in BOTH apps (the run-8 cull/clear quirks were real but secondary).
Fix: live client-sent FOV when valid (validated at ingest,
`sane_fov`), signed-symmetric {-0.86,+0.86,+0.86,-0.86} fallback
otherwise; streamer sanitizes pre-connect app FOVs the same way
(shared `fallback_fov`/`sane_fov` in runtime.h). Demo also fixed:
explicit viewport/scissor, LOAD→CLEAR, Vulkan Y-negation in
projFromFov (upright through the snapshot pipeline); invertRigid
re-derived elementwise — already correct R^T, untouched. Headless
pixel proof (`WIVRN_MACOS_VERIFY_PIXELS=1`, EyeSnapshot readback at
frame 30): diff=20563 (1.96%) gTop=2562818 gBot=0 → cube draws
upright. test_runtime now asserts FOV signs + finite projection.
Awaiting retest of both apps on headset.
hello_xr cross-validation: stock hello_xr cannot render on this MoltenVK
(ClearAttachments yields driver fill; BACK+CW cubes fully culled — both
proven in isolation). `patches/hello_xr/` carries two MoltenVK workarounds
applied at fetch time via the same apply.sh helper as patches/monado:
0001 loadOp-CLEAR + CULL_NONE in vulkan_utils.h, 0002 drops the
vkCmdClearAttachments block in graphicsplugin_vulkan.cpp (loadOp-CLEAR
cover it via clearValues). Clean-fetch verified (wiped prefix rebuilds
patched from the pinned tarball). Awaiting patched-hello_xr headset run
(static colored cubes + grip cubes = independent-app proof).
- [ ] `video_encoder_x264` + `raw` via `encode(info, sink)` adapter;
  host-visible copy verified modifier-free on MoltenVK.
- [ ] `accept_connection` adapter (9757), `send_shard` sink, IDR
  handler unchanged.
- [ ] `bonjour_publisher.cpp` (`dns_sd.h`, same TXT keys).
- [ ] Foveation compute + squasher GPU submit (moved from Phase 4:
  only exercisable against real pixels) + pacer presentation
  feedback.

Accept: `xrEndFrame` → squash → x264 → 9757 → client → poses back →
`xrWaitFrame` prediction loop closes.

## Phase 6 — Bring-up + proof [status: headless done, headset pending]

- [x] `hello_xr` (Vulkan/MoltenVK) via `XR_RUNTIME_JSON` streams live
  cube to headset.
- [x] Headless prep: stock hello_xr 1.1.59 via isolated
  ExternalProject (`WIVRN_BUILD_MACOS_HELLO_XR`, trimmed to samples;
  same-scope FetchContent impossible — its `openxr_loader` target
  collides with ours). Session-state events (IDLE+READY at create;
  hello_xr waits for READY), 11 hello_xr-needed entry points
  (view-config props, LocateSpaces, bounds rect, profile/name stubs,
  StructureTypeToString, 7 debug-utils stubs), BGRA/RGBA swizzle in
  the NV12 converter, EndFrame frame counter. Headless: 17,700 stereo
  frames, 0 errors (`--graphics Vulkan2`, enable2 runtime-created
  device path).
- [ ] Live headset run (instructions with user; headset ready).

Accept: live cube, pose-tracked views. No real-game compat, no CTS.

Findings: hello_xr `--graphics Vulkan` (DIY device) fails in ITS OWN
vkCreateInstance (no portability-enumeration bit) — use Vulkan2
(runtime-created instance/device, fully covered); enable2 needs
`xrGetVulkanGraphicsDevice2KHR` too; `xrCreateInstance` must be
NULL-retrievable else the loader jumps to 0; unknown-extension
lookups only log (apps crash on null, not on errors); hello_xr exits
on stdin EOF — pipe `sleep` to keep it alive for smoke runs;
`WIVRN_MACOS_HEADLESS=1` keeps headless runs from binding/advertising.

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
