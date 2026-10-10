# WiVRn mask perf — what actually mattered

Measured on Quest (90Hz, 11.1ms budget; eye 1680x1760, mask 2496x2624) via
per-group GPU timestamp brackets + per-stage CPU clocks (`mask perf:`
logcat line, Statistics tab). Mixed = 2 groups (f=12+f=24 or 23+24).

## Cost model (use this before optimizing)

- Cost ≈ **pass count x pass size**. Each fullscreen pass pays a ~fixed
  tile store/load round-trip (~0.3-0.5ms); shader fetches are second-order
  (5-tap saved ~30% on tier-1, ~0% on tier-2).
- **Swapchain (XR-shared, uncached) round-trips dominate**: V-upscale
  writes + compositor reads. Intermediates (cached/compressed) are cheap
  in comparison — shrinking them 4x bought ~15%.
- **Positional tail penalty under saturation**: with 2 stacks the second
  recorded group cost ~4ms extra regardless of feather/size/passes. It is
  NOT intrinsic — vanishes once total traffic drops under the wall.
- Record order = `passthrough_objects` map order (object id); logged as
  `mask groups order:`. Slow-slot questions are unanswerable without it.
- XR acquire order is load-bearing: member swapchains must be acquired
  AFTER the video swapchain each frame. Member-before-video wedges member
  pools within seconds (`xrWait/AcquireSwapchainImage: CALL_ORDER_INVALID`
  forever; ~6 healthy frames then dead; video unaffected). Submit reorder
  is therefore unavailable as a phase lever — record order implies
  acquire order (record needs its image).
- 5s means hide regimes; per-window **minima** reveal them (2ms dip =
  skipped group; 5ms spike ≠ skip). `groups min` separates the two.

## What worked (mixed 8.0 -> 0.85ms)

1. **Half-res member swapchains + compositor upscale** (the breakthrough,
   ~8x): V writes half pixels, runtime bilinear-upscales (free,
   equivalent filtering). Tier-0 keeps full (exact, 0.08ms anyway).
2. **Collapse Stage-1** (8->6 passes tiered): seed-rasterize at level 1
   directly; same-group 4.2->1.45ms combined with below.
3. **5-tap kernel** (9->5 taps, 3 fetches, renormalized): ~30% off
   fetch-bound passes. Keep spreads calibrated per tier (sigma match).
4. **Tier-0 raster-direct**: deleted the identity copy pass (was pure
   waste: spread-0 blur); tier-0 now ~0.08ms, leave it alone.
5. **The scoreboard itself**: per-group GPU brackets, CPU clocks, order
   log, min/max/min-groups. Every finding above came from it. Keep it.
6. **Quarter-res tiered submit** (tier-0 full, tier-1 half, tier ≥ 2
   quarter): V already abstracted submit size via `out_extent`, so zero
   shader changes — sizing-only diff. Tier-2 means ~halved, saturated
   mixed totals 6→~4ms. Constraint learned: submit scale must resolve
   the band (~0.24f texels across it); tier-1 at quarter would put f=7's
   band in 0.4 texels — broken, not soft.
7. **Spread continuity** (f/12,f/44 → f/24,f/48,f/48,f/96: divisor 12k):
   tier-1's slope was ~2x the 2/4/8 family (~40% pop at f=16→17); band
   is now √2·f/6 at every tier, exact at all boundaries.

## What didn't (or barely)

- **SDF full replacement** (reverted): 5.1x single-group, ~1x mixed.
  Jump Flood passes cost more than gaussian at headset resolutions;
  keep gaussian for small feathers. Lesson: match algorithm to regime.
- **Intermediate shrinking** (half-A/B, quarter-tier-2, RG16F): each
  helped its tier ~2x solo but barely moved mixed totals — the wall was
  swapchain traffic, not intermediates. Shrink outputs first.
- **Punch scissor**: correct but debug-only; negligible in numbers.
- **Single-composite unification**: saves ~2 full passes (~1ms) but not
  the saturation mechanism. Downgraded; per-object feathers are now
  required, so quantize is off the table — headroom comes from tiers +
  variance control instead.
- **Tier-1 subpass merge** (deleted after measuring ≈identical twice):
  raster+H+V in one pass with DONT_CARE intermediates bought nothing.
  Reason: our blurs need neighborhood taps (sampler reads), and Vulkan
  transient images forbid sampler reads — so the driver gets no
  transiency signal and resolves through DRAM anyway. Subpass fusion
  needs pixel-local (input-attachment) reads; neighborhood filters can't
  use them. (Secondary: at half-res the round-trips were only ~0.1ms —
  the model over-promised.)
- **Cull hysteresis** (reverted unvalidated): linger-≤3-frames on
  disappear, instant appear. Sound design, zero measurement behind it;
  stateless skip restored until flap-rate data justifies it.
- **Theories ruled out**: descriptor aliasing (real bug, fixed, but it
  caused wrong-output not slowness); DVFS (10x too big for clocks);
  marker flicker (dips persist with steady QR); mesh size (same model
  fast-when-first, slow-when-second); swapchain image counts
  (runtime-managed, properly waited).

## Stutter chase (spike forensics — the ceiling, not the mean)

- Headroom is a **statistics problem**: means (~2-4ms) leave budget at
  every refresh rate; tails (6-9ms) + variance do the damage. Minimize
  P(total > budget) *and* frame-time variance. Felt stutter tracks
  **mask variance**, not video spikes: AG@120 (86 video-only spikes,
  mask ~0.07 flat) felt perfect; AF@72 (M1 + flapping) didn't. Working
  model: timewarp covers late video, but a late/swimming passthrough
  cutout is felt directly.
- **M1** (above) persists: first recorded slot eats ~2.5-3.5ms under
  saturation regardless of content. Slow slot follows record position.
- **M2 video-region spikes** (~5.5ms in slots 0→1 = blits+defoveate):
  blits ≈ 0.00 always, all defoveate; mask brackets normal in the same
  frames; waits/submit/endframe flat; no cadence break on spike frames;
  cold-present, motion-free, starved-0, mask-inactive frames spike too.
  Per-eye split (built, measured, reverted) showed e0-only/e1-only/
  balanced mix — no first-work ordering. Clocks sampled over spikes show
  no dip pattern (spikes avoid low clocks in one config, bimodal in
  another). Surviving model: bursty DRAM contention (decoder/camera/
  compositor traffic) hitting whichever eye's pass executes through the
  burst; duration scales with total load (balanced under feathered load,
  per-eye-random when light).
- **Forensics retention** (cheap, permanent alibis): `mask spike:` dump
  (whole+mask triggers 5.0/4.0ms), `mask gap:` epoch-guard line,
  `misses/spikes/starved/vr mean/max` counters, video-region split
  (blits/defov), cadence misses (idle gaps excluded), wait/submit/
  endframe clocks, CPU stage clocks. Deleted after serving: merged path,
  per-eye split, bypass switch, hysteresis.

## Benchmark discipline (hard-won)

- Matrix: solo per tier + mixed + same-feather; 60s runs, fixed
  resolution/refresh/markers; overlays off; ignore first log line.
- `tier=` field meanings changed across commits (blur 0/1/2/4/8, then
  SDF divs, then blur again) — check which commit a log came from.
- Resolution constancy: eye + mask swapchain sizes are in every log
  (`Creating new swapchain`, `Fiducial mask swapchain`) — verify before
  comparing across runs.
- Tripwires per commit: name the control that must NOT move (e.g.
  tier-1 ~2.2ms during tier-2 work) or the refactor leaked.
- Matrix now mixed-band (10/26/67) + refresh ladder (72/90/120),
  overlays noted per run: rate (content/behavior), impact (budget) and
  config (mask variance) confound freely — AG@120 vs AF@72 proved it.
- Spike lines need the `blits + defov == video` identity check (readback
  health). `draws=` is window-max (not last). `misses` excludes idle
  gaps (early-out re-arm). Thresholds live in two consts
  (`spike_threshold_ms`, `spike_mask_threshold_ms`).
