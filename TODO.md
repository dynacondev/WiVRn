# Mask perf sequence (benchmark E-matrix between each; revert individually on regression)

- [x] 1. R8 intermediates — A/B/D/E to R8_UNORM, RGBA8 submits stay
- [x] 2. Frustum skip — AABB at upload, per-draw NDC test + feather margin, filter before acquire
- [x] 3. Eye-outer reorder — {raster,H,V} per eye instead of per pass
- [ ] 4. Mask last — block to just before submit
- [ ] 5. Mask first — block to right after cmd begin
- [x] 6. CPU C0 — tracker/sync timers in mask perf line (measure first)
- [ ] 7. CPU C1 — stagger + back off snapshots (round-robin 1 tracker/frame, slow when held-stable)
- [ ] 8. CPU C2 — fingerprint gate (rebuild map_key/uni/exists only on source change)
- [ ] 9. CPU C3 — scratch reuse + string copies (reserve vectors, string_view payloads, no oid copies)
- [ ] Staged (not scheduled): flatten (single shared mask, additive V), quantize (one stack)

In progress: C0 + #2 frustum skip.
Tripwires: tier-1 control must not move unexpectedly; any visual delta on untouched paths = leakage, stop.
