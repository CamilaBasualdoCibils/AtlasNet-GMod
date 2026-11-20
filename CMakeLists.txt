cmake_minimum_required(VERSION 3.24)

set(atlasnet_sdk "${CMAKE_CURRENT_SOURCE_DIR}/AtlasNet-SourceBridge/AtlasNet")
if(NOT EXISTS "${atlasnet_sdk}/CMakeLists.txt")
  message(FATAL_ERROR "Initialize AtlasNet with git submodule update --init --recursive")
endif()

# Use the SDK manifest when the caller configures with the vcpkg toolchain.
set(VCPKG_MANIFEST_DIR "${atlasnet_sdk}" CACHE PATH "AtlasNet vcpkg manifest directory")

project(AtlasNetGMod VERSION 0.1.0 LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" OR
    NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "AtlasNet-GMod targets Linux x86-64")
endif()
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
  message(FATAL_ERROR "Use a single-configuration generator, such as Ninja")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(cmake/FacepunchModuleBase.cmake)

function(add_atlasnet_sdk)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(ATLASNET_EXAMPLES OFF)
  set(ATLASNET_TESTS OFF)
  set(ATLASNET_DEV OFF)
  set(ATLASNET_JAVABINDINGS OFF)
  set(ATLASNET_GOBJECT OFF)
  set(ATLASNET_STEAMNETSOCK OFF)
  # Upstream enables its shared Tracy profiler in Debug. Keep the embedded
  # SDK profiler-free while allowing a Debug build of the GMod module.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
  add_subdirectory("${atlasnet_sdk}" atlasnet EXCLUDE_FROM_ALL)
endfunction()
add_atlasnet_sdk()

add_library(atlasnet_gmod MODULE src/Server/ServerModule.cpp src/Server/GatewayLua.cpp src/Server/SimulationLua.cpp)
target_compile_features(atlasnet_gmod PRIVATE cxx_std_23)
target_compile_definitions(atlasnet_gmod PRIVATE
  GMMODULE ATLASNET_GMOD_VERSION="${PROJECT_VERSION}")
target_include_directories(atlasnet_gmod SYSTEM PRIVATE "${facepunch_include}")
target_link_libraries(atlasnet_gmod PRIVATE AtlasNetCore)
# Export only the two GMod entry points, including when a static dependency
# was built with default visibility.
target_link_options(atlasnet_gmod PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
set_target_properties(atlasnet_gmod PROPERTIES
  PREFIX ""
  OUTPUT_NAME gmsv_atlasnet_linux64
  SUFFIX ".dll"
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

target_include_directories(atlasnet_gmod PRIVATE src)

add_library(atlasnet_source1 MODULE src/Endpoint/SourceEndpoint.cpp src/Endpoint/ChatShard.cpp src/Simulation/GModShard.cpp src/Simulation/Process.cpp)
target_compile_features(atlasnet_source1 PRIVATE cxx_std_23)
target_compile_definitions(atlasnet_source1 PRIVATE ATLASNET_GMOD_VERSION="${PROJECT_VERSION}")
target_include_directories(atlasnet_source1 PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(atlasnet_source1 PRIVATE AtlasNetModule Threads::Threads)
target_link_options(atlasnet_source1 PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
set_target_properties(atlasnet_source1 PROPERTIES PREFIX "" CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# Build the SDK's example heuristic with the exact Module SDK used by our host.
set(CMAKE_CXX_STANDARD 23)
add_subdirectory("${atlasnet_sdk}/Examples/Heuristics/XAxisSplit" xsplit EXCLUDE_FROM_ALL)
