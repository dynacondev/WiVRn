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

## What didn't (or barely)

- **SDF full replacement** (reverted): 5.1x single-group, ~1x mixed.
  Jump Flood passes cost more than gaussian at headset resolutions;
  keep gaussian for small feathers. Lesson: match algorithm to regime.
- **Intermediate shrinking** (half-A/B, quarter-tier-2, RG16F): each
  helped its tier ~2x solo but barely moved mixed totals — the wall was
  swapchain traffic, not intermediates. Shrink outputs first.
- **Punch scissor**: correct but debug-only; negligible in numbers.
- **Single-composite unification**: saves ~2 full passes (~1ms) but not
  the saturation mechanism. Downgraded; quantize (one shared stack) is
  the structural hammer if mixed ever regresses.
- **Theories ruled out**: descriptor aliasing (real bug, fixed, but it
  caused wrong-output not slowness); DVFS (10x too big for clocks);
  marker flicker (dips persist with steady QR); mesh size (same model
  fast-when-first, slow-when-second); swapchain image counts
  (runtime-managed, properly waited).

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
