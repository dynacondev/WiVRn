# AGENTS.md — WiVRn agent notes

Lab notebook for coding agents working on this repo. Append lessons that cost real device round-trips.

## Fullscreen black + per-frame "Invalid pose submitted": check GUI quads first (Quest 3)

- Symptom: persistent full-frame black (video *and* overlay GUI gone), log shows `Invalid pose submitted` every frame, recovery as soon as GUI hides. Head tracking fine, no swapchain/LUT errors. First found via a combo dropdown whose popup overlapped the tooltip-strip viewport.
- Cause: `imgui_impl::end_frame()` (`client/render/imgui_impl.cpp`) emits one `XrCompositionLayerQuad` per viewport overlapped by any active window. A viewport whose `position`/`orientation` was never assigned (default `glm::quat` is *not* zero-initialized — reads as `(0,0,0,0)` in practice) produces a zero-quaternion quad. One bad layer fails the whole `xrEndFrame` with `XR_ERROR_POSE_INVALID`, so a single bad GUI quad blacks out everything, every frame the overlap persists.
- Concrete instance: stream GUI viewports (`client/scenes/stream.cpp:556`) — main `[0]` posed every frame, popup `[2]` placed via `place_layer_relative`, but tooltip strip `[1]` relied solely on the per-tooltip hook. Any non-tooltip window crossing its image rect (e.g. a low combo popup) emitted the poison quad. High combos worked (popup stayed inside viewport `[0]`), which misdirected toward the LUT code.
- Rules:
  1. Every viewport in `layers_` must hold a valid pose on *every* frame it could overlap a window — place fallback poses up front (see `place_layer_relative(1, 0, ...)` in `stream_gui.cpp`), refine per-window after (tooltip hook pattern).
  2. `end_frame()` now skips non-normalized viewport poses with a one-shot warn instead of submitting — never let one bad quad fail the frame.
  3. Prefer non-popup selectors (‹ › steppers, radios) for controls near the bottom of scrollable pages: combo popups open downward past the panel into the next viewport's image rect and split across quads (chopped, detached look even when poses are valid).
  4. Diagnose with the `POSE_INVALID diag` block (`stream::log_pose_invalid_diagnostics`): it names the exact layer (`quad BAD ... quat=(0,0,0,0)`), view flags, and popup-open state. `popup_open=true` + a quad with zero quat = this bug, not tracking loss.
- Related trap: the same "everything black" signature previously also came from `xrReleaseSwapchainImage ... CALL_ORDER_INVALID` after a throw between `begin_frame`/`end_frame` skipped the submit (valueless `passthrough` variant poisoned every later frame). If the diag block is *absent* and the error is `CALL_ORDER_INVALID` instead, suspect frame-loop poisoning (`session::enable_passthrough` emplace guard), not poses.

## Adreno: never use `mat3` constructors/ternaries in fragment shaders (Quest 3, Adreno 740, driver 512.837.9)

- `mat3(vec4, vec4, vec4)` (and the `(cond) ? mat3 : mat3` ternary) **miscompiles** in a multiview fragment shader: the resulting matrix reads back as garbage/zero on device with no error, no validation complaint, no crash.
- Symptom: everything downstream of the matrix goes dark (e.g. `p.z <= 0` guards fire on every pixel). CPU-side logging looks perfect, GPU timestamps show the pass executing, merge/seed/layers all proven — only the `mat3` math is wrong.
- Diagnostic trap: any echo/viz gated behind `mat3`-derived values (even a constant echo placed *after* the guards) also reads black, which falsely implicates the data transport (UBO/push/texel buffer all looked "dead" here while likely fine). **Always validate a new data path with an unconditional constant output placed *before* any computed guards.**
- Rule: spell small matrix math as **plain scalar dot products** (`float px = c0.x * uv.x + ...`), select per-eye columns with integer arithmetic (`int e = gl_ViewIndex * 3`), no ternaries on matrices.
- Proven-good read units in this driver path, in order of evidence: `sampler2DArray` texture taps, per-eye UBO (see `depth_overlay`), scalar push constants in the vertex stage. Unproven here (all masked by the above): fragment-stage UBO/push/texel-buffer reads — treat as guilty until proven innocent with a guard-free echo.
