cmake_minimum_required(VERSION 3.20)
project(makeexr VERSION 1.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# One self-contained executable: OpenEXR, Imath and libdeflate are built from source and
# linked statically; with MSVC the C runtime is linked statically too (no VC++ redistributable).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(BUILD_WEBSITE OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(openexr
  URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.3.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openexr)

add_executable(makeexr src/makeexr.cpp src/radiance.cpp)
target_link_libraries(makeexr PRIVATE OpenEXR::OpenEXR)
if(MSVC)
  target_compile_options(makeexr PRIVATE /W3 /utf-8)
  target_compile_definitions(makeexr PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(makeexr PRIVATE -Wall -Wextra)
endif()

option(MAKEEXR_BUILD_TESTS "Build the exrdump test helper" OFF)
if(MAKEEXR_BUILD_TESTS)
  add_executable(exrdump tests/exrdump.cpp)
  target_link_libraries(exrdump PRIVATE OpenEXR::OpenEXR)
endif()
