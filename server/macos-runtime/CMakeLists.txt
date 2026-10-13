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

# x264 software encoder (brew x264; v1 hardware path is VideoToolbox in v2).
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)

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
	vk_context.cpp
	graphics.cpp
	swapchain.cpp
	frame.cpp
	snapshot.cpp
	encode.cpp
	net.cpp
	streamer.cpp
	bonjour.cpp
)
target_include_directories(wivrn-macos-runtime PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-runtime PRIVATE Vulkan::Vulkan wivrn-common PkgConfig::X264)
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

# Phase 4 proof: headless MoltenVK frame-loop drive through the real
# loader (instance extensions + device bring-up + swapchain cycles +
# Wait/Begin/EndFrame), with validation layers when available.
add_executable(wivrn-macos-graphics-test
	test_graphics.cpp
)
target_include_directories(wivrn-macos-graphics-test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-graphics-test PRIVATE openxr_loader Vulkan::Vulkan)

# Phase 5a proof: clear-to-encode pixels-to-bits (no loader/headset).
add_executable(wivrn-macos-encode-test
	test_encode.cpp
	runtime.cpp
	snapshot.cpp
	encode.cpp
)
target_include_directories(wivrn-macos-encode-test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-encode-test PRIVATE Vulkan::Vulkan PkgConfig::X264)

# Phase 5b proof: full handshake + steady-state exchange against a
# loopback fake headset (no real hardware).
add_executable(wivrn-macos-net-test
	test_net.cpp
	runtime.cpp
	net.cpp
	vk_context.cpp
)
target_include_directories(wivrn-macos-net-test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-net-test PRIVATE Vulkan::Vulkan wivrn-common)

# Phase 5d proof: full pixels-to-wire-to-poses loopback (fake headset +
# real runtime dylib + in-process mini app). Needs XR_RUNTIME_JSON and
# must NOT set WIVRN_MACOS_HEADLESS (streaming is the point).
add_executable(wivrn-macos-stream-test
	test_stream.cpp
)
target_include_directories(wivrn-macos-stream-test PRIVATE
	${openxr_loader_SOURCE_DIR}/include
	.
)
target_link_libraries(wivrn-macos-stream-test PRIVATE openxr_loader Vulkan::Vulkan wivrn-common)

# Minimal animated demo app (v1 headset proof vehicle): cycling background
# + spinning cube through our swapchains. Uses only verified paths
# (loadOp-CLEAR, no depth/culling/ClearAttachments); hello_xr cannot serve
# as the proof (its ClearAttachments yields driver fill + its BACK+CW
# cubes are culled on MoltenVK — see ROADMAP run 8).
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cubec.vert.spv ${CMAKE_CURRENT_BINARY_DIR}/cubec.frag.spv
		COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cubec.vert -o ${CMAKE_CURRENT_BINARY_DIR}/cubec.vert.spv
		COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cubec.frag -o ${CMAKE_CURRENT_BINARY_DIR}/cubec.frag.spv
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cubec.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/cubec.frag
		COMMENT "Compiling demo shaders"
	)
	add_executable(wivrn-macos-demo
		demo.cpp
		${CMAKE_CURRENT_BINARY_DIR}/cubec.vert.spv
		${CMAKE_CURRENT_BINARY_DIR}/cubec.frag.spv
	)
	target_include_directories(wivrn-macos-demo PRIVATE
		${openxr_loader_SOURCE_DIR}/include
		.
	)
	target_link_libraries(wivrn-macos-demo PRIVATE openxr_loader Vulkan::Vulkan)
else()
	message(STATUS "glslangValidator not found, skipping wivrn-macos-demo")
endif()

# Phase 6: stock hello_xr from OpenXR-SDK-Source, built isolated via
# ExternalProject (its loader target would collide with ours in one
# scope). Trimmed to samples only; default graphics overridden on the
# command line (--graphics Vulkan) at run time.
option(WIVRN_BUILD_MACOS_HELLO_XR "Build stock hello_xr for headset testing" OFF)
if(WIVRN_BUILD_MACOS_HELLO_XR)
	include(ExternalProject)
	ExternalProject_Add(hello_xr_external
		URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/release-1.1.59.tar.gz
		URL_HASH SHA256=3b241897f93e2cf7e833d27f8ec3c3433cf5cf064e13724af026984acede02e7
		CMAKE_ARGS
			-DBUILD_TESTS=ON
			-DBUILD_CONFORMANCE_TESTS=OFF
			-DBUILD_API_LAYERS=OFF
			-DBUILD_WITH_SYSTEM_JSONCPP=OFF
			-DCMAKE_BUILD_TYPE=Release
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target hello_xr --config Release
		INSTALL_COMMAND ""
		BUILD_BYPRODUCTS <BINARY_DIR>/src/tests/hello_xr/hello_xr
	)
	ExternalProject_Get_Property(hello_xr_external BINARY_DIR)
	message(STATUS "hello_xr builds to: ${BINARY_DIR}/src/tests/hello_xr/hello_xr")
endif()
