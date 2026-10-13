# macOS bespoke server (v1 MVP) — isolated from Linux server/ + Monado.
# Upstream-clean rule: no includes of xrt_*/comp_*/ipc_*/u_* here. Reuse
# common/ (packets/serialization/crypto) by link only, via thin adapters
# defined in this folder (see ROADMAP.md "macOS Server v1 MVP").
#
# v1 architecture: the runtime dylib IS the server core, hosted in the
# OpenXR application process (no service/IPC split). The headset connects
# directly to the application process; wivrn-server-macos is the
# launcher/headless companion (full daemon role arrives in Phase 5+).

include(FetchContent)

# Same revision the client uses (top-level declares it; populate here —
# the client tree is OFF in this preset so this is the first populate).
FetchContent_GetProperties(openxr_loader)
if(NOT openxr_loader_POPULATED)
	FetchContent_MakeAvailable(openxr_loader)
endif()

# Runtime dylib, loaded by the Khronos loader via wivrn-macos.json.
# Headers come straight from the SDK include dir; the runtime must NOT
# link the loader itself (the loader dlopens the runtime, not vice versa).
add_library(wivrn-macos-runtime MODULE
	runtime.cpp
	loader_entry.cpp
	instance.cpp
	system.cpp
	views.cpp
	session.cpp
	tracking.cpp
	spaces.cpp
	actions.cpp
)
target_include_directories(wivrn-macos-runtime PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
set_target_properties(wivrn-macos-runtime PROPERTIES
	PREFIX ""
	SUFFIX ".dylib"
)

# Loader manifest: absolute library_path keeps dev runs hermetic
# (XR_RUNTIME_JSON=<build>/wivrn-macos.json, no install needed).
set(WIVRN_MACOS_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/wivrn-macos-runtime.dylib")
configure_file(wivrn-macos.json.in wivrn-macos.json @ONLY)

# Launcher / headless companion executable (stub until Phase 5+).
add_executable(wivrn-server-macos
	main.cpp
)

# Phase 2 proof: drive the real Khronos loader against our manifest and
# exercise negotiate -> CreateInstance -> GetSystem -> views -> session.
add_executable(wivrn-macos-runtime-test
	test_runtime.cpp
)
target_include_directories(wivrn-macos-runtime-test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-runtime-test PRIVATE openxr_loader)
