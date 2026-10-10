# Mask perf sequence (benchmark E-matrix between each; revert individually on regression)

- [x] 1. R8 intermediates — A/B/D/E to R8_UNORM, RGBA8 submits stay
- [x] 2. Frustum skip — AABB at upload, per-draw NDC test + feather margin, filter before acquire
- [x] 3. Eye-outer reorder — {raster,H,V} per eye instead of per pass
- [ ] 4. Mask last — SKIPPED as no-op (mask already records last; only debug fill follows)
- [x] 5. Mask first — block before defoveate (real phase shift; frozen-safe outside timewarp-if)
- [x] 6. CPU C0 — tracker/sync timers in mask perf line (measure first)
- [ ] 7. CPU C1 — stagger + back off snapshots (round-robin 1 tracker/frame, slow when held-stable)
- [ ] 8. CPU C2 — fingerprint gate (rebuild map_key/uni/exists only on source change)
- [ ] 9. CPU C3 — scratch reuse + string copies (reserve vectors, string_view payloads, no oid copies)
- [x] 10. Eager setup — swapchain + targets warmed at map arrival, record stays draws-gated
- [ ] Staged (not scheduled): flatten (single shared mask, additive V), quantize (one stack)

In progress: next benchmark (E-matrix + look-away + first-visible behavior).
Tripwires: tier-1 control must not move unexpectedly; any visual delta on untouched paths = leakage, stop.
