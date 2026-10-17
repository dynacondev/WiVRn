# AGENTS.md — WiVRn agent notes

Lab notebook for coding agents working on this repo. Append lessons that cost real device round-trips.

## Adreno: never use `mat3` constructors/ternaries in fragment shaders (Quest 3, Adreno 740, driver 512.837.9)

- `mat3(vec4, vec4, vec4)` (and the `(cond) ? mat3 : mat3` ternary) **miscompiles** in a multiview fragment shader: the resulting matrix reads back as garbage/zero on device with no error, no validation complaint, no crash.
- Symptom: everything downstream of the matrix goes dark (e.g. `p.z <= 0` guards fire on every pixel). CPU-side logging looks perfect, GPU timestamps show the pass executing, merge/seed/layers all proven — only the `mat3` math is wrong.
- Diagnostic trap: any echo/viz gated behind `mat3`-derived values (even a constant echo placed *after* the guards) also reads black, which falsely implicates the data transport (UBO/push/texel buffer all looked "dead" here while likely fine). **Always validate a new data path with an unconditional constant output placed *before* any computed guards.**
- Rule: spell small matrix math as **plain scalar dot products** (`float px = c0.x * uv.x + ...`), select per-eye columns with integer arithmetic (`int e = gl_ViewIndex * 3`), no ternaries on matrices.
- Proven-good read units in this driver path, in order of evidence: `sampler2DArray` texture taps, per-eye UBO (see `depth_overlay`), scalar push constants in the vertex stage. Unproven here (all masked by the above): fragment-stage UBO/push/texel-buffer reads — treat as guilty until proven innocent with a guard-free echo.
