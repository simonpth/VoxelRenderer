cmake_minimum_required(VERSION 3.25)

project(VoxelRenderer
	VERSION 0.1.0
	DESCRIPTION "A Vulkan voxel rendering engine"
	LANGUAGES CXX
)

include(FetchContent)
include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	message(FATAL_ERROR
		"SDL3 requires Apple Clang on macOS. Reconfigure from a clean build directory with "
		"CC=clang CXX=clang++ cmake -S . -B build"
	)
endif()

option(VOXEL_BUILD_TESTS "Build unit tests" OFF)
option(VOXEL_ENABLE_TRACY "Build Tracy profiling support" OFF)
option(VOXEL_ENABLE_LTO "Enable link-time optimization in Release builds" ON)
option(VOXEL_ENABLE_SLANG "Compile Slang shaders at build time" ON)

if(VOXEL_ENABLE_LTO)
	check_ipo_supported(RESULT VOXEL_IPO_SUPPORTED OUTPUT VOXEL_IPO_ERROR)
endif()

# Vulkan is intentionally found from the system SDK. Install the LunarG SDK
# (or a distribution equivalent) and set VULKAN_SDK when CMake cannot find it.
find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

if(VOXEL_ENABLE_SLANG)
	find_program(SLANGC_EXECUTABLE slangc HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)
endif()

set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG release-3.4.16
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	volk
	GIT_REPOSITORY https://github.com/zeux/volk.git
	GIT_TAG vulkan-sdk-1.4.357.0
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	vk_bootstrap
	GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
	GIT_TAG v1.4.364
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	VulkanMemoryAllocator
	GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
	GIT_TAG v3.4.0
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 1.0.3
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	clay
	GIT_REPOSITORY https://github.com/nicbarker/clay.git
	GIT_TAG v0.14
	GIT_SHALLOW TRUE
)

FetchContent_Declare(
	stb
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
	GIT_SHALLOW TRUE
)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
set(CLAY_INCLUDE_ALL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(
	SDL3
	volk
	vk_bootstrap
	VulkanMemoryAllocator
	glm
	clay
)

if(NOT TARGET clay)
	add_library(clay INTERFACE)
	target_include_directories(clay INTERFACE "${clay_SOURCE_DIR}")
endif()

FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
	if(POLICY CMP0169)
		cmake_policy(SET CMP0169 OLD)
	endif()
	FetchContent_Populate(stb)
	add_library(stb INTERFACE)
	target_include_directories(stb INTERFACE "${stb_SOURCE_DIR}")
endif()

if(VOXEL_ENABLE_TRACY)
	set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
	set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
	FetchContent_Declare(
		tracy
		GIT_REPOSITORY https://github.com/wolfpld/tracy.git
		GIT_TAG v0.12.2
		GIT_SHALLOW TRUE
	)
	FetchContent_MakeAvailable(tracy)
endif()

set(COMPUTE_SLANG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/compute.slang")
set(COMPUTE_SLANG_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/svo.slang")
set(FULLSCREEN_SLANG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/fullscreen.slang")
set(OVERLAY_SLANG_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/overlay.slang")
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(VERTEX_SHADER "${SHADER_OUTPUT_DIR}/triangle.vert.spv")
set(FRAGMENT_SHADER "${SHADER_OUTPUT_DIR}/triangle.frag.spv")
set(COMPUTE_SHADER "${SHADER_OUTPUT_DIR}/pixel.comp.spv")
set(OVERLAY_VERTEX_SHADER "${SHADER_OUTPUT_DIR}/overlay.vert.spv")
set(OVERLAY_FRAGMENT_SHADER "${SHADER_OUTPUT_DIR}/overlay.frag.spv")

if(VOXEL_ENABLE_SLANG)
	add_custom_command(
		OUTPUT "${VERTEX_SHADER}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
		COMMAND "${SLANGC_EXECUTABLE}" -target spirv -profile spirv_1_3
			-entry vertexMain -stage vertex -o "${VERTEX_SHADER}" "${FULLSCREEN_SLANG_SOURCE}"
		DEPENDS "${FULLSCREEN_SLANG_SOURCE}"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${FRAGMENT_SHADER}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
		COMMAND "${SLANGC_EXECUTABLE}" -target spirv -profile spirv_1_3
			-entry fragmentMain -stage fragment -o "${FRAGMENT_SHADER}" "${FULLSCREEN_SLANG_SOURCE}"
		DEPENDS "${FULLSCREEN_SLANG_SOURCE}"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${COMPUTE_SHADER}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
		COMMAND "${SLANGC_EXECUTABLE}" -target spirv -profile spirv_1_3
			-entry computeMain -stage compute -o "${COMPUTE_SHADER}" "${COMPUTE_SLANG_SOURCE}"
		DEPENDS "${COMPUTE_SLANG_SOURCE}" "${COMPUTE_SLANG_DEPS}"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${OVERLAY_VERTEX_SHADER}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
		COMMAND "${SLANGC_EXECUTABLE}" -target spirv -profile spirv_1_3
			-entry overlayVertexMain -stage vertex -o "${OVERLAY_VERTEX_SHADER}" "${OVERLAY_SLANG_SOURCE}"
		DEPENDS "${OVERLAY_SLANG_SOURCE}"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${OVERLAY_FRAGMENT_SHADER}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUTPUT_DIR}"
		COMMAND "${SLANGC_EXECUTABLE}" -target spirv -profile spirv_1_3
			-entry overlayFragmentMain -stage fragment -o "${OVERLAY_FRAGMENT_SHADER}" "${OVERLAY_SLANG_SOURCE}"
		DEPENDS "${OVERLAY_SLANG_SOURCE}"
		VERBATIM
	)
else()
	message(FATAL_ERROR "VOXEL_ENABLE_SLANG=OFF requires precompiled shader files, which are not configured")
endif()

add_executable(VoxelRenderer
	src/main.cpp
	src/config.cpp
	src/renderer/vulkancontext.cpp
	src/renderer/ui.cpp
	src/simulation/authority.cpp
	src/simulation/octree.cpp
	src/simulation/provenance.cpp
	src/simulation/region.cpp
	src/simulation/lod.cpp
	src/simulation/lod_rings.cpp
	src/simulation/brick_pool.cpp
	src/simulation/svo.cpp
	src/network/protocol.cpp
	src/network/udp_socket.cpp
	src/network/tcp_socket.cpp
	src/network/server.cpp
	src/network/client.cpp
	"${VERTEX_SHADER}"
	"${FRAGMENT_SHADER}"
	"${COMPUTE_SHADER}"
	"${OVERLAY_VERTEX_SHADER}"
	"${OVERLAY_FRAGMENT_SHADER}"
)

target_include_directories(VoxelRenderer PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(VoxelRenderer PRIVATE
	SDL3::SDL3
	Vulkan::Vulkan
	volk::volk
	vk-bootstrap::vk-bootstrap
	GPUOpen::VulkanMemoryAllocator
	glm::glm
	clay
	stb
)


target_compile_definitions(VoxelRenderer PRIVATE
	GLM_FORCE_RADIANS
	GLM_FORCE_DEPTH_ZERO_TO_ONE
	VULKAN_HPP_NO_CONSTRUCTORS
	VOXEL_SHADER_DIR="${SHADER_OUTPUT_DIR}/"
)

if(VOXEL_ENABLE_TRACY)
	target_link_libraries(VoxelRenderer PRIVATE Tracy::TracyClient)
	target_compile_definitions(VoxelRenderer PRIVATE TRACY_ENABLE)
endif()

if(MSVC)
	target_compile_options(VoxelRenderer PRIVATE /W4 /permissive-)
else()
	target_compile_options(VoxelRenderer PRIVATE -Wall -Wextra -Wpedantic)
	endif()

if(VOXEL_ENABLE_LTO AND VOXEL_IPO_SUPPORTED)
	set_property(TARGET VoxelRenderer PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
	endif()

if(APPLE)
	# SDL3 uses MoltenVK through the Vulkan SDK on macOS.
	target_compile_definitions(VoxelRenderer PRIVATE VK_USE_PLATFORM_METAL_EXT)
endif()

if(VOXEL_BUILD_TESTS)
	enable_testing()

	# Tests are written with assert() and are worthless if it is compiled out.
	# A release build type adds -DNDEBUG globally, which would leave every
	# target below "passing" while checking nothing -- a failure mode that is
	# invisible precisely because ctest still reports success.
	#
	# This is worth being explicit about: the whole suite was silently inert
	# under the default RelWithDebInfo configuration before this was added.
	add_compile_options($<$<CONFIG:Debug,RelWithDebInfo>:-UNDEBUG>)

	# Simulation core, shared by every test target below.
	set(VOXEL_SIM_SOURCES
		src/simulation/octree.cpp
		src/simulation/provenance.cpp
		src/simulation/region.cpp
		src/simulation/lod.cpp
		src/simulation/lod_rings.cpp
		src/simulation/brick_pool.cpp
		src/simulation/svo.cpp
	)

	add_executable(VoxelSvoTests
		tests/svo_test.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelSvoTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelSvoTests PRIVATE glm::glm)
	add_test(NAME VoxelSvoTests COMMAND VoxelSvoTests)

	add_executable(VoxelPersistTests
		tests/persist_test.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelPersistTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelPersistTests PRIVATE glm::glm)
	add_test(NAME VoxelPersistTests COMMAND VoxelPersistTests)

	add_executable(VoxelLodTests
		tests/lod_test.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelLodTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelLodTests PRIVATE glm::glm)
	add_test(NAME VoxelLodTests COMMAND VoxelLodTests)

	add_executable(VoxelProtocolTests
		tests/protocol_test.cpp
		src/network/protocol.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelProtocolTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelProtocolTests PRIVATE glm::glm)
	add_test(NAME VoxelProtocolTests COMMAND VoxelProtocolTests)

	add_executable(VoxelAuthorityTests
		tests/authority_test.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelAuthorityTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelAuthorityTests PRIVATE glm::glm)
	add_test(NAME VoxelAuthorityTests COMMAND VoxelAuthorityTests)

	add_executable(VoxelLifecycleTests
		tests/lifecycle_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelLifecycleTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelLifecycleTests PRIVATE glm::glm)
	add_test(NAME VoxelLifecycleTests COMMAND VoxelLifecycleTests)

	# End to end at the production world size (65536 x 65536 x 8192), where the
	# root layer is 16,777,216 entries and the server generates lazily.
	add_executable(VoxelBigWorldTests
		tests/bigworld_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelBigWorldTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelBigWorldTests PRIVATE glm::glm)
	add_test(NAME VoxelBigWorldTests COMMAND VoxelBigWorldTests)

	# The LOD pyramid: each level exactly 1/8 the one below, uniform bricks
	# unchanged, and averages taken over level-0 geometry rather than guessed.
	add_executable(VoxelLodPyramidTests
		tests/lod_pyramid_test.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelLodPyramidTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelLodPyramidTests PRIVATE glm::glm)
	add_test(NAME VoxelLodPyramidTests COMMAND VoxelLodPyramidTests)

	# Token bucket pacing, directly: deterministic, no sockets, no terrain.
	add_executable(VoxelPacerTests
		tests/pacer_test.cpp
	)
	target_include_directories(VoxelPacerTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	add_test(NAME VoxelPacerTests COMMAND VoxelPacerTests)

	# Bricks must arrive finest detail first: the offered ring sequence is
	# non-decreasing, which is what "high quality before low quality" means.
	add_executable(VoxelStreamOrderTests
		tests/stream_order_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelStreamOrderTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelStreamOrderTests PRIVATE glm::glm)
	add_test(NAME VoxelStreamOrderTests COMMAND VoxelStreamOrderTests)

	# Pins the streaming delivery contract: no acknowledgement before upload, no
	# silent drop when the queue is full, retry on silence, and redelivery after
	# a brick leaves and re-enters the interest window.
	add_executable(VoxelStreamTests
		tests/stream_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelStreamTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelStreamTests PRIVATE glm::glm)
	add_test(NAME VoxelStreamTests COMMAND VoxelStreamTests)

	# Pins the ring-to-level mapping and measures what the server actually
	# puts on the wire. Without the rings, every brick arrives at full
	# resolution and the bandwidth saving does not exist.
	add_executable(VoxelLodRingTests
		tests/lod_rings_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelLodRingTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelLodRingTests PRIVATE glm::glm)
	add_test(NAME VoxelLodRingTests COMMAND VoxelLodRingTests)

	# Does W travel where the camera points? Regression test for the change
	# from move -- raw WASD, yaw-relative -- to moveDirection, resolved by the
	# client. Measured end to end because the property under test is that the
	# wire direction and the drawn direction agree.
	add_executable(VoxelDirectionTests
		tests/direction_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelDirectionTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelDirectionTests PRIVATE glm::glm)
	add_test(NAME VoxelDirectionTests COMMAND VoxelDirectionTests)

	# Pins the bounded arena: that a run is trimmed when its brick's level
	# drops, that a full arena evicts the least recently wanted brick, and that
	# recency is updated on re-claim. Without the trim the arena fills with
	# held-but-unused runs and storage is unbounded in practice, however large
	# the budget is.
	add_executable(VoxelBrickPoolTests
		tests/brick_pool_test.cpp
		src/simulation/brick_pool.cpp
	)
	target_include_directories(VoxelBrickPoolTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelBrickPoolTests PRIVATE glm::glm)
	add_test(NAME VoxelBrickPoolTests COMMAND VoxelBrickPoolTests)

	# Pins eviction recovery end to end: an evicted brick must leave the
	# client's resident set and must be retracted at the server, or its next
	# resend is dismissed as a duplicate and the terrain never comes back. The
	# arena is deliberately far smaller than the window so eviction is the
	# steady state rather than an edge case.
	add_executable(VoxelLruTests
		tests/lru_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelLruTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelLruTests PRIVATE glm::glm)
	add_test(NAME VoxelLruTests COMMAND VoxelLruTests)

	# Pins that the server streams the *whole world*, not just the neighbourhood,
	# and streams it over time rather than as one burst on connect. Without this
	# an arena larger than the interest window has nothing to hold.
	add_executable(VoxelStreamAllTests
		tests/stream_all_test.cpp
		src/network/client.cpp
		src/network/server.cpp
		src/network/protocol.cpp
		src/network/tcp_socket.cpp
		src/network/udp_socket.cpp
		src/simulation/authority.cpp
		${VOXEL_SIM_SOURCES}
	)
	target_include_directories(VoxelStreamAllTests PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
	)
	target_link_libraries(VoxelStreamAllTests PRIVATE glm::glm)
	add_test(NAME VoxelStreamAllTests COMMAND VoxelStreamAllTests)
endif()
