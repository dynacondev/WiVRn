# macOS server — on-headset A/B runbook (artifacts first)

On-headset procedure for the macOS (`feat/macos`) server tree. Every
knob, command, and log pattern below was verified by grep against the
merged tree; the grep hit backs each entry. Unverified items are flagged
`[UNVERIFIED]` — do not trust them.

Scope: `server/macos-runtime/` (bespoke macOS runtime) + headset client
log lines + `tools/analyze_stream_log.py`. Linux `server/` behavior is
referenced only where the macOS code comments cite it.

## 0. Build the final tree

```bash
cmake --preset macos-server          # preset name + binaryDir build-macos-server: CMakePresets.json:87-90
cmake --build build-macos-server
```

Binaries (server/macos-runtime/CMakeLists.txt:76,162):

- `wivrn-server-macos` — launcher. Usage
  `wivrn-server-macos [--port PORT] [--runtime PATH] -- APP [ARGS...]`
  (server/macos-runtime/main.cpp:25). Port travels to the runtime via
  `WIVRN_PORT` env; manifest via `XR_RUNTIME_JSON`
  (server/macos-runtime/main.cpp:50,90).
- `wivrn-macos-demo` — headless-capable test VR app
  (`wivrn-server-macos -- wivrn-macos-demo`, server/macos-runtime/demo.cpp:299).

Launch for headset runs (streaming lives in-process in APP):

```bash
./build-macos-server/server/macos-runtime/wivrn-server-macos -- ./build-macos-server/server/macos-runtime/wivrn-macos-demo
```

## 1. Toggle matrix

All knobs read at runtime; **unset == fixed default behavior** (rollback
= unset everything). Grouping follows the operation's workstreams.

### VT encoder (`server/macos-runtime/encode_vt.h:11-31` header, read at open() in `encode_vt.cpp:143-149`)

| knob | default | effect when changed |
|---|---|---|
| `WIVRN_MACOS_VIDEO_ENCODER` (`encode.cpp:30`) | auto = VT hardware first, x264 fallback | `x264` / `vt` forces a backend |
| `WIVRN_MACOS_VT_IDR_EVERY` | 120 (~1.3 s @90fps; `encode_vt.cpp:24`) | N = periodic IDR every N feeds; `0` = off (forced/NACK IDRs unaffected) |
| `WIVRN_MACOS_VT_IDR_ON_DROP` | on (`true`) | `=0` disables recovery IDR after a locally dropped frame |
| `WIVRN_MACOS_VT_FAST` | on (`true`) | `=0` disables `PrioritizeEncodingSpeed` hint (`encode_vt.cpp:228`) |
| `WIVRN_MACOS_VT_COLOR` | on (`true`) | `=0` disables x264-matching VUI signaling (709/sRGB/full-range) |
| `WIVRN_MACOS_VT_FPS` | 0 = headset refresh from params | N overrides the `ExpectedFrameRate` hint (PTS timescale always params fps) |
| `WIVRN_MACOS_VT_SLICE_BYTES` | 0 = off | N = best-effort `MaxH264SliceBytes` (opt-in: transport has no per-slice recovery, `encode_vt.cpp:256`) |
| `WIVRN_MACOS_VT_FEED_TIMEOUT_MS` | 500 | callback wait bound; `0` forces timeouts (diagnostic only) |

### Transport (`server/macos-runtime/net.cpp:132-141` header, applied `net.cpp:1005-1006,404`)

| knob | default | effect when changed |
|---|---|---|
| `WIVRN_MACOS_PACED_SEND` | 1 (on) | `=0` disables UDP shard pacing (all shards back-to-back) |
| `WIVRN_MACOS_SHARD_BURST` | 8 (clamped 1..256) | N = max shards sent back-to-back before a pacing slice |
| `WIVRN_MACOS_TCP_ONLY` | 0 (off) | `=1` forces TCP-only video path (stream shards over control; `net.cpp:407` logs `link: TCP-only video forced by WIVRN_MACOS_TCP_ONLY`) |

### Pipeline (`server/macos-runtime/streamer.cpp:24-49`, `frame.cpp:89-97`)

| knob | default | effect when changed |
|---|---|---|
| `WIVRN_MACOS_BITRATE_SPLIT` | on | `=0` disables per-eye halving (each eye gets the full bitrate → ~2× wire total) |
| `WIVRN_MACOS_PAIR_GATE_US` | 1500 µs | stereo-pair gate bound; `0` disables the wait |
| `WIVRN_MACOS_WAITFRAME_BLOCK` | on (blocking) | `=0` disables the sleep-to-predicted-time (app free-runs; overflows latest-only mailbox) |

### Harness / headless (test + demo only — not headset knobs)

| knob | default | effect when changed |
|---|---|---|
| `WIVRN_MACOS_STREAM_PHASES` (`test_stream.cpp:1861`) | `loop` | `all` (fork-per-phase) or comma subset of `tear,skew,loop` (`test_stream.cpp:12-26,1867-1903`) |
| `WIVRN_MACOS_DEMO_GEN_TAG` (`demo.cpp:315`) | off | paints frame-seq tag into a 32×32 corner block (tear hook; output pixel-identical when off) |
| `WIVRN_MACOS_SKEW_BOUND_MS` (`test_stream.cpp:1282`) | 2.0 | overrides the skew-soak pass bound |
| `WIVRN_MACOS_HEADLESS` (`frame.cpp:150`, `session.cpp:114`) | off | when set: no port bind, no Bonjour, no streaming (EndFrame skips `Streamer`) |
| `WIVRN_MACOS_VERIFY_PIXELS` (`demo.cpp:308`) | off | headless pixel proof (asserts cube drew; +`DEMO_GEN_TAG` runs the corner self-check) |

### Pre-existing / environment

| knob | default / note |
|---|---|
| `WIVRN_PORT` (`streamer.cpp:70`) | 9757 |
| `WIVRN_HOSTNAME` (`streamer.cpp:76`) | `WiVRn` (Bonjour name; tests use `WiVRn-Tear`/`WiVRn-Skew`) |
| `XR_RUNTIME_JSON` (`main.cpp:50`, `demo.cpp:296`) | manifest path; launcher sets it for the child |
| `VK_ICD_FILENAMES` (`main.cpp:96-98`) | launcher pins Homebrew MoltenVK unless already set |
| `WIVRN_DUMP_VIDEO` | **[NOT WIRED into macOS runtime]** — only hit is Linux `server/encoder/video_encoder.cpp:164`. Dumping sent frames on macOS is not available; use client-side observation + analyzer instead. |

Discovery: Bonjour `_wivrn._tcp` (`bonjour.h:1`, `bonjour.cpp:48,64`;
replaces Avahi on macOS). Ports 9757 TCP+UDP (`main.cpp:51`,
`streamer.h:35`). Same-version server/APK required (protocol check
`net.cpp:559` drops on mismatch).

## 2. Baseline procedure

1. Build (§0) and launch the server (§0). Confirm
   `bonjour: advertising 'WiVRn' _wivrn._tcp:9757` (`bonjour.cpp:64`)
   and `link: timesync converged, offset=...` (`net.cpp:223`) — no
   timesync line means timesync responses never arrive.
2. Headset package/intent (docs/debugging.md:109-137,264-274):
   - Variants: `org.meumeu.wivrn[.local|.github|.github.testing|.github.nightly]`.
   - `WIVRN_PKG=$(adb shell pm list packages | grep wivrn | cut -d: -f2)`
    - USB baseline first (the intent below points at headset-side
      `localhost:9757`, which exists ONLY through the reverse tunnel —
      without it the client fails with "server refused the connection"
      even though the server is fine. Create AND verify the tunnel):
      ```bash
      adb reverse tcp:9757 tcp:9757
      adb reverse --list   # must show ... tcp:9757 tcp:9757 ...; if empty, the tunnel failed (cable/USB mode/daemon) — fix that first
      adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost:9757" ${WIVRN_PKG}
      ```
   - Then WiFi runs (same intent with the Mac's LAN address).
3. Client log filter: `adb logcat '*:S' WiVRn:V` (docs/debugging.md:70).
   Watch for (all grep-verified):
   - `frame {} was not sent with {} data shards, ... missing` (`client/decoder/shard_accumulator.cpp:103`)
   - `frame {} was not sent because no shard was received` (`client/decoder/shard_accumulator.cpp:85`)
   - `Failed to find a common frame for all decoders, ...` (`client/scenes/stream.cpp:720`)
   - `Fiducial mask groups order: ...` (`client/scenes/stream.cpp:1557`) for record order.
   - `[UNVERIFIED on-headset]:` `mask perf:` / `mask spike:` bracket lines are documented in AGENTS.md from Quest profiling, not grep-verified in `client/` for this path — confirm presence in your logcat before relying on them.
4. Server lines to watch (all grep-verified):
   - `streamer: eye %u encoded=%llu ... ow=%llu sk=%llu ed=%llu aq=%llu tk=%llu gated=%llu solo=%llu idrcd=%llu` (`streamer.cpp:773`) — per-eye heartbeat: encodes, drops, snapshot/encode ms, worker counters.
   - `vt: hardware H.264 session open (%ux%u @%u, %u bps, idr_every=%u, timeout_ms=%u)` (`encode_vt.cpp:323`) — confirms VT (absent = x264 fallback; see also `video-encoder: VT unavailable, falling back to x264`, `encode.cpp:46`).
   - `link: fb stream=%u frame=%llu recv=... todec=...` (`net.cpp:732`) — per-frame journey; `todec!=0` counts as completed (analyzer).
   - `link: NACK stream=%u frame=%llu, forcing IDR` (`net.cpp:743`) vs `... suppressed (stale|covered|idr in flight|cooldown)` (`net.cpp:935,947,966,978`).
   - `vt: frame dropped (reason=timeout|callback ...)` (`encode_vt.cpp:577,590`).
   - `link: net sent=%llu drop=%llu dshard=%llu estall=%llu esaved=%llu idr_force=%llu idr_supp=%llu` (`net.cpp:1106`) — aggregate heartbeat.
   - `vt: fps hint mismatch`, `vt: slice support (...)`, `vt: speed hint (...)`, `vt: color (...)` (`encode_vt.cpp:115,129,232,249`) — confirm hint application per run.

## 3. A/B sequence (artifacts first)

Fixed scene, ~60 s per run, overlays off. Save both sides every run:
server stdout → `server.log`, `adb logcat '*:S' WiVRn:V` → `run.log`.
Score each run with (CLI usage string `tools/analyze_stream_log.py:13-14`):

```bash
python3 tools/analyze_stream_log.py server.log run.log
# usage: tools/analyze_stream_log.py [--kind auto|server|client] FILE [FILE ...]
# auto-detects server lines (streamer: eye / link: NACK) vs client lines (was not sent / common frame).
# Prints per-file NACK-rate per 100 encodes, IDR-burst/storm flags, incomplete-frame and no-common-frame counts, plus an aggregate.
```

Run order (isolate one fix per comparison; defaults first):

| # | A vs B | targets (symptom) | observe |
|---|---|---|---|
| 0 | baseline: all defaults, USB | reference | analyzer totals; note `encoded`, NACK/100enc, incomplete |
| 1 | `WIVRN_MACOS_BITRATE_SPLIT=0` vs default 1 | over/under-bitrate: macroblocking vs softness | wire total doubles with `=0`; `link: fb`/`net` counters; client incomplete deltas |
| 2 | `WIVRN_MACOS_PACED_SEND=0` vs default 1 | WiFi burst loss / stutter | NACK bursts + `suppressed (cooldown)` clustering; try `SHARD_BURST=1` vs `32` as follow-up |
| 3 | `WIVRN_MACOS_VIDEO_ENCODER=x264` vs default auto(VT) | encoder-attributable artifacts/latency | `vt: hardware ... open` present/absent; `streamer: eye ... enc=` ms; `vt: frame dropped` only on VT side |
| 4 | `WIVRN_MACOS_VT_IDR_EVERY=0` vs default 120 | smearing after loss (recovery time) | time-to-clean after a NACK; `link: NACK ... forcing IDR` rate |
| 5 | `WIVRN_MACOS_VT_IDR_ON_DROP=0` vs default on | smearing after local drops | correlate with `vt: frame dropped` lines — smear should linger with `=0` |
| 6 | `WIVRN_MACOS_TCP_ONLY=1` (USB) vs default | loss vs latency isolation | NACKs should ~vanish on TCP; if stutter remains it is not packet loss |
| 7 | `WIVRN_MACOS_PAIR_GATE_US=0` vs default 1500 | one-eye lag / stereo mismatch | `Failed to find a common frame` rate; per-eye `idx` divergence in `streamer: eye` lines |
| 8 | `WIVRN_MACOS_VT_SLICE_BYTES=1400` vs default 0 | large-frame loss on WiFi | incomplete-frame count; check `vt: slices (max_bytes=... applied=...)` for whether the HW honored it |
| 9 | `WIVRN_MACOS_WAITFRAME_BLOCK=0` vs default on | app-rate vs headset-rate (stutter source) | `streamer: eye ... drop=` / `sk=` mailbox-drop counters |
| 10 | WiFi repeat of winner | final validation | re-run §2 + analyzer; keep both logs as the sign-off artifact |

One-knob-at-a-time; record the exact env per run in the log filename
(`run03-vt-x264.log`). A run that changes two knobs is not evidence.

## 4. Triage matrix

| symptom | likely domain | confirming log line / toggle |
|---|---|---|
| tearing (image split mid-frame) | snapshot GPU sync (A3) | server `streamer: eye ... snap=` spikes; headless proof is the `tear` harness phase (`test_stream.cpp:12-17`); no client-side torn-pair line exists in-tree besides the `common frame` fallback (§2) |
| smearing (corruption lingers) | IDR recovery (VT/NACK) | `link: NACK ... forcing IDR` missing → check `suppressed (...)` reason; test `VT_IDR_EVERY=120` vs `0`, `VT_IDR_ON_DROP` on vs `0` |
| stutter (hitches, poses fine) | pacing / app rate / pair gate | `link: net ... estall=`; `streamer: eye ... drop=/sk=`; toggles `PACED_SEND`, `WAITFRAME_BLOCK`, `PAIR_GATE_US` |
| macroblock artifacts | bitrate / encoder | per-eye bitrate: `BITRATE_SPLIT=0` vs 1; backend: `VIDEO_ENCODER=x264` vs VT; `enc=` ms in heartbeat |
| frozen frames | feed timeout / TCP stall | `vt: frame dropped (reason=timeout ...)` + `timeout_ms=` in the open line; `TCP_ONLY=1` isolates loss vs stall |
| one-eye lag | eye fairness / pair gate | `Failed to find a common frame` rate; per-eye `encoded/idx` divergence; `PAIR_GATE_US=0` vs 1500; headless proof is the `skew` phase (bound 2.0 ms, `test_stream.cpp:1262-1283`) |

## 5. Sign-off sheet

- [ ] §3 runs 0–10 each have `server.log` + `run.log` artifacts and an analyzer line recorded.
- [ ] VT default path shows `vt: hardware H.264 session open` with `idr_every=120, timeout_ms=500`; x264 fallback path shows the fallback line — both deliberate, never accidental.
- [ ] NACK-rate/100enc + incomplete-frames for the winning config are at or below the run-0 baseline on both USB and WiFi.
- [ ] `Failed to find a common frame` count is at/near zero on the winning config.
- [ ] No `[UNVERIFIED]` item above was relied on without first confirming it in-capture.

Rollback: all toggles default to fixed behavior — `unset` every
`WIVRN_MACOS_*` (and `WIVRN_PORT`/`WIVRN_HOSTNAME` overrides) to return
to shipped behavior. To revert to the pre-operation build WITHOUT
touching this checkout (it may hold uncommitted state), use a spare
worktree that reuses the shared dependency cache:

```bash
git worktree add ../WiVRn-baseline bbad2e41
export CCACHE_DIR=/Users/luke/Documents/Projects/WiVRn.worktrees/_ccache
cmake --preset macos-server "-DFETCHCONTENT_BASE_DIR=/Users/luke/Documents/Projects/WiVRn.worktrees/_fetchcontent-shared" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build-macos-server
```
(run the last two with `../WiVRn-baseline` as the working directory;
remove it afterwards with `git worktree remove ../WiVRn-baseline`.)

## 6. Known headless-vs-headset gaps (do not over-claim lab results)

Sourced from code comments/test headers, not from headset data:

- Tear detector measures **snapshot purity only**: every capture must
  carry exactly one generation through the real `EyeSnapshot` path
  (`test_stream.cpp:12-17`). The client's torn-pair / `common frame`
  fallback (`client/scenes/stream.cpp:720`) is a separate mechanism —
  a passing `tear` phase says nothing about it.
- Skew loopback uses a **forced-x264 overload**
  (`::setenv("WIVRN_MACOS_VIDEO_ENCODER", "x264", 1)` so "the encode
  cost (and the lag it induces) is deterministic", `test_stream.cpp:1268-1281`).
  Production VT is faster per frame but more overloaded overall — the
  2.0 ms bound does not transfer 1:1 to VT on-headset.
- **MoltenVK FIFO masks queue-order races on this host**: same-queue
  submissions execute in order (`snapshot.h:22-29`), so the copy
  cannot overtake the render here even where the fence edge is weak;
  a headset-path GPU (or any multi-queue future) would not be so
  forgiving. Fence timeouts LOG and PROCEED (`snapshot.h:30-33`) — a
  torn frame, not a stall.
- **Dither ±1 LSB on clears**: bg vs corner clears round apart by ≤1
  LSB across MoltenVK's fullscreen vs scissored clear paths, absorbed
  by a ±2 compare window; generations stride by 37 so real mixes land
  ≥4 LSB apart (`test_stream.cpp:1081-1087,1117-1123`). Faint
  single-LSB headset corruption is below this detector's floor by design.
