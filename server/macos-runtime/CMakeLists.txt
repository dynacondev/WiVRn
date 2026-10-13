# macOS bespoke server (v1 MVP) — isolated from Linux server/ + Monado.
# Upstream-clean rule: no includes of xrt_*/comp_*/ipc_*/u_* here. Reuse
# common/ (packets/serialization/crypto) by link only, via thin adapters
# defined in this folder (see ROADMAP.md "macOS Server v1 MVP").

add_executable(wivrn-server-macos
    main.cpp
)

target_compile_features(wivrn-server-macos PRIVATE cxx_std_20)

# common/ linkage comes in Phase 1 once the portability shims land.
# install(TARGETS wivrn-server-macos) arrives with Phase 7 packaging.
