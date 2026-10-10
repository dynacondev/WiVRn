# AGENTS.md — WiVRn

FOSS PCVR streamer: Linux PC runs VR apps, streams to Android headset.
`server/` PC OpenXR runtime (Monado fork) · `client/` headset app ·
`dashboard/` Qt6/Kirigami GUI · `common/` shared protocol ·
`tools/wivrnctl` systemd helper · `tools/perfetto/` tracing ·
`tools/wireshark/` dissector.

## Build

Use presets (Ninja, `Debug`+`WERROR=ON` by default); never hand-roll
`-DWIVRN_BUILD_*` flags when a preset fits. Local builds are
client-only — do NOT build `server` or `dashboard` on this machine
(they build on a separate Linux machine); Android client builds are fine here:

```bash
cmake --preset client && cmake --build build-client       # linux debug client only (local OK)
# macOS local configure needs homebrew ffmpeg visible + no pipewire/system-openxr/system-ktx:
# export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:$PKG_CONFIG_PATH"
# cmake --preset client -DWIVRN_USE_PIPEWIRE=OFF -DWIVRN_USE_SYSTEM_OPENXR=OFF -DWIVRN_USE_SYSTEM_LIBKTX=OFF
# (macOS host build itself is NOT green: pre-existing Linux-isms in common/
# + LunarG /usr/local SDK vs -Wundef. Validate client code via
# ./gradlew assembleDebug instead; add -DCMAKE_CXX_FLAGS="-Wno-error=undef"
# to keep host configure useful.)
./gradlew assembleRelease  # real headset APK, needs ANDROID_HOME, Java 17, ks.keystore + signingKeyPassword in gradle.properties (local OK)
# Remote-only (separate Linux machine, reference — do not run here):
# cmake --preset server && cmake --build build-server
# cmake --preset dashboard -DWIVRN_BUILD_SERVER=ON && cmake --build build-dashboard
# cmake --preset server-tracing && cmake --build build-server-tracing  # Perfetto, see docs/profiling.md
```

Details in `docs/building.md`. Server needs Vulkan ≥1.4.304 and ≥1
encoder (`WIVRN_USE_NVENC/VAAPI/VULKAN_ENCODE/X264`); client build needs
`rsvg-convert`, `ktx` CLI, `glslangValidator`; `WIVRN_COMPRESS_GLB=ON`
additionally needs `gltf-transform`. Client requires Boost ≥1.84 with
`url` component (server-only: 1.75+). `WIVRN_USE_SYSTEM_*=OFF` fetches
bundled deps instead of system ones. Mutually exclusive CMake pairs
fail configure: `GIT_TAG` vs `GIT_DESC`/`GIT_COMMIT`,
`WIVRN_OPTIMIZE_SHADERS` vs `WIVRN_DEBUG_SHADERS`,
`WIVRN_TRACE_MONADO=ON` requires `WIVRN_USE_PERFETTO=ON` (+ system
percetto, Monado never fetches it).

Monado is pinned: rev in `monado-rev`, patches in `patches/monado/`
applied at FetchContent time. Never edit fetched Monado sources; fix
in `patches/monado/` + bump nothing (rev file drives refetch).
Flatpak manifest is generated: `tools/gen_flatpak_manifest.py --gitlocal`.

## Check

```bash
clang-format --dry-run -Werror <file>  # C++ style: tabs, width 8 (.clang-format); CI checks client server dashboard common tools/wireshark with clang-format 22
ruff check && ruff format --check       # only lints tools/**/*.py (ruff.toml)
cmake --preset server -DWIVRN_BUILD_TEST=ON && cmake --build build-server --target list-apps vdf  # only test binaries in repo (common/ Steam/VDF helpers) — remote-only, do not run here
```

No `ctest` suite (`BUILD_TESTING=OFF` for Monado, no top-level tests).
Dashboard compiles QML via `qt_add_qml_module` — QML errors surface at
build time, read them there. `WIVRN_WERROR=ON` is default in presets;
CI builds `Release`.

## Run / debug

Server and headset APK must be same version or connection fails.
Ports: 9757 TCP+UDP (WiVRn), 5353/UDP (Avahi, must be running).
Config: `docs/configuration.md`; files later in list win:
`/usr/share/wivrn/config.json` → `/etc/wivrn/config.json` →
`$XDG_CONFIG_HOME/wivrn/config.json`. Flatpak config lives under
`~/.var/app/io.github.wivrn.wivrn/`.

```bash
XRT_LOG=debug XRT_COMPOSITOR_LOG=debug wivrn-server   # server logs
WIVRN_DUMP_VIDEO=/tmp/vdump wivrn-server              # dump sent frames, play with mpv
adb logcat '*:S' WiVRn:V                              # headset logs; '*:F' for crashes
adb reverse tcp:9757 tcp:9757 && adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost:9757" $(adb shell pm list packages | grep wivrn | cut -d: -f2)  # USB
```

APK package per variant: `.local` local, `.github` release,
`.github.testing` CI, `.github.nightly` nightlies, no suffix = store
(`docs/debugging.md`). Tracing: `WIVRN_TRACING=inprocess|system`,
inert unless set; full flow in `docs/profiling.md`.

## Conventions

- i18n: `tools/update_messages.sh [lang]` regenerates `locale/`; client
  uses gettext `_()`/`_F()`, dashboard uses `i18n()` in QML/C++.
- QR fiducials: one payload string = one identity. Same payload printed
  twice is NOT two trackables; for multi-location coverage print
  distinct payloads and link one object to several fiducial ids
  (`docs/configuration.md#fiducials-and-passthrough`).
- Marker offsets are authored tag-in-board (tag pose in model coords;
  position edits move along world axes, orientation tweaks pivot about
  the tag) and inverted once at load — never hand-invert in config.
  Object offsets are object-in-board, used directly.
- `common/wivrn_packets.h` + `wivrn_serialization*.h` define the
  wire protocol — keep client/server in sync; no compat layer.
- Board fusion: `client/xr/board_solver.{h,cpp}` (Eigen 3.4.0 +
  manif 0.0.5, both FetchContent; tiny_solver vendored in
  `client/thirdparty/`). Single-node SE(3), LOO-subset selection,
  trimmed cost, Cauchy IRLS. Gradle builds `targets "wivrn"` only so
  dep test/bench trees never build. In-app self-test button in the
  Passthrough tab; `board solve:` logcat lines are the tuning
  instrument. Pose filtering/smoothing TEMP-disabled (files stay,
  unreferenced) until fusion is verified on-device.
- Debug overlays (`client/render/debug_lines.*`): all gizmos share ONE
  transparent projection layer (never per-quad layers — those blow
  `maxLayerCount`). Geometry is thin triangles (line topology never
  rasterized on the Quest path — do not reintroduce it); own swapchain,
  CLEAR-on-UNDEFINED every frame (never LOAD an eye image: its layout
  is unobservable without validation layers).

## Mask perf (active feat/markerboard work — keep)

Measured on Quest (90Hz; eye 1680x1760, mask 2496x2624) via per-group
GPU brackets + per-stage CPU clocks (`mask perf:` logcat line).
Mixed = 2 groups (f=12+f=24 or 23+24).

- Cost ≈ **pass count × pass size**. Fullscreen passes pay ~0.3–0.5ms
  tile round-trip; shader fetches second-order (5-tap saved ~30%
  tier-1, ~0% tier-2). **Swapchain (XR-shared, uncached) traffic
  dominates**; shrinking cached intermediates 4× bought ~15%.
- **XR acquire order is load-bearing**: member swapchains must be
  acquired AFTER the video swapchain each frame. Member-before-video
  wedges member pools within seconds (`CALL_ORDER_INVALID` forever).
  Submit reorder is therefore unavailable as a lever — record order
  implies acquire order.
- Fixes that moved mixed 8.0→0.85ms: half-res member swapchains +
  compositor upscale (~8×); seed-rasterize at level 1 (8→6 passes);
  5-tap kernel (keep sigma-matched spreads); tier-0 raster-direct
  (~0.08ms, leave alone).
- Later, saturated regime: **quarter-res tiered submit** (tier-0 full,
  tier-1 half, tier ≥2 quarter; sizing-only via `out_extent`, zero
  shader changes; saturated mixed 6→~4ms). Constraint: submit scale
  must resolve the band (~0.24 texels across it); tier-1 at quarter
  puts f=7's band in 0.4 texels — broken, not soft.
- **Spread continuity** (divisor 12k: f/24,f/48,f/48,f/96): tier-1's
  slope was ~2× the 2/4/8 family (~40% pop at f=16→17); band is now
  √2·f/6 at every tier, exact at all boundaries.
- Dead ends: SDF/Jump-Flood replacement (reverted, 5.1× solo cost);
  intermediate shrinking; punch scissor (debug-only); descriptor
  aliasing/DVFS/marker-flicker/mesh-size theories ruled out.
  Single-composite unification saves ~2 passes but not the saturation
  mechanism (downgraded; per-object feathers rule out quantize —
  headroom is tiers + variance control). Tier-1 subpass merge
  (raster+H+V, DONT_CARE intermediates) measured ≈identical twice,
  deleted: neighborhood taps need sampler reads, transients forbid
  them, driver resolves through DRAM anyway. Cull hysteresis
  (linger-3/appear-instant) reverted unvalidated — needs flap-rate
  data first.
- Discipline: solo-per-tier + mixed + same-feather matrix, 60s runs,
  fixed res/refresh/markers, overlays off, ignore first log line.
  `tier=` meanings changed across commits — check commit before
  comparing logs. Verify res constancy via `Creating new swapchain` /
  `Fiducial mask swapchain` lines. Record order = `passthrough_objects`
  map order (`mask groups order:` log); per-window minima (`groups min`)
  separate skipped groups (2ms dips) from spikes. Matrix now
  mixed-band (10/26/67) + refresh ladder (72/90/120), overlays noted
  per run (rate/impact/config confound freely). Spike lines need the
  `blits + defov == video` identity check; `draws=` is window-max;
  thresholds in `spike_threshold_ms` / `spike_mask_threshold_ms`.

## Stutter chase (spike forensics — ceiling, not mean)

- Headroom is a **statistics problem**: means (~2-4ms) leave budget at
  every refresh; tails (6-9ms) + variance do the damage. Felt stutter
  tracks **mask variance**, not video spikes (AG@120: 86 video-only
  spikes, mask ~0.07 flat, felt perfect; AF@72 with M1 + flapping
  didn't). Timewarp covers late video; a late/swimming cutout is felt.
- **M1**: first recorded slot eats ~2.5-3.5ms under saturation
  regardless of content; slow slot follows record position.
- **M2 video-region spikes** (~5.5ms in slots 0→1 = blits+defoveate):
  blits ≈ 0.00 always, all defoveate; mask brackets normal in the same
  frames; waits/submit/endframe flat; no cadence break; cold-present,
  motion-free, starved-0 frames spike too. Surviving model: bursty DRAM
  contention hitting whichever eye's pass executes through the burst.
- **Forensics retention** (cheap, permanent alibis): `mask spike:` dump
  (whole+mask triggers 5.0/4.0ms), `mask gap:` epoch-guard line,
  `misses/spikes/starved/vr mean/max` counters, video-region split
  (blits/defov), cadence misses (idle gaps excluded), wait/submit/
  endframe clocks, CPU stage clocks. Deleted after serving: merged
  path, per-eye split, bypass switch, hysteresis.
