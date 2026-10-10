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
- `common/wivrn_packets.h` + `wivrn_serialization*.h` define the
  wire protocol — keep client/server in sync; no compat layer.

## Mask perf (active feat/markerboard work — keep)

Measured on Quest (90Hz; eye 1680x1760, mask 2496x2624) via per-group
GPU brackets + per-stage CPU clocks (`mask perf:` logcat line).
Mixed = 2 groups (f=12+f=24 or 23+24).

- Cost ≈ **pass count × pass size**. Fullscreen passes pay ~0.3–0.5ms
  tile round-trip; shader fetches second-order (5-tap saved ~30%
  tier-1, ~0% tier-2). **Swapchain (XR-shared, uncached) traffic
  dominates**; shrinking cached intermediates 4× bought ~15%.
- Fixes that moved mixed 8.0→0.85ms: half-res member swapchains +
  compositor upscale (~8×); seed-rasterize at level 1 (8→6 passes);
  5-tap kernel (keep sigma-matched spreads); tier-0 raster-direct
  (~0.08ms, leave alone).
- Dead ends: SDF/Jump-Flood replacement (reverted, 5.1× solo cost);
  intermediate shrinking; punch scissor (debug-only); descriptor
  aliasing/DVFS/marker-flicker/mesh-size theories ruled out.
- Discipline: solo-per-tier + mixed + same-feather matrix, 60s runs,
  fixed res/refresh/markers, overlays off, ignore first log line.
  `tier=` meanings changed across commits — check commit before
  comparing logs. Verify res constancy via `Creating new swapchain` /
  `Fiducial mask swapchain` lines. Record order = `passthrough_objects`
  map order (`mask groups order:` log); per-window minima (`groups min`)
  separate skipped groups (2ms dips) from spikes.
