cmake_minimum_required(VERSION 3.20)
project(Primordia VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PRIMORDIA_NATIVE "Tune the build for this machine's CPU (-march=native)" ON)
option(PRIMORDIA_SANITIZE "Build Primordia's own code with AddressSanitizer + UBSan" OFF)

find_package(SDL3 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
find_package(Threads REQUIRED)

# --- Dear ImGui (docking) + ImPlot -------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp
  ${IMPLOT_DIR}/implot_demo.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMPLOT_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_compile_options(imgui PRIVATE -O2 -w)

# --- Primordia ---------------------------------------------------------------
file(GLOB_RECURSE PRIMORDIA_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(primordia ${PRIMORDIA_SOURCES})
set(PRIMORDIA_HIDDEN 16 CACHE STRING "Hidden neurons per brain (16, 32, 64...)")
target_compile_definitions(primordia PRIVATE PRIMORDIA_HIDDEN=${PRIMORDIA_HIDDEN})
target_include_directories(primordia PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(primordia PRIVATE imgui SDL3::SDL3 PkgConfig::EPOXY Threads::Threads)
target_compile_options(primordia PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -fopenmp-simd
  $<$<CONFIG:Release>:-O3>
  $<$<BOOL:${PRIMORDIA_NATIVE}>:-march=native>
)

if(PRIMORDIA_SANITIZE)
  target_compile_options(primordia PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(primordia PRIVATE -fsanitize=address,undefined)
endif()

# Ship fonts next to the binary so the app runs from the build directory.
add_custom_command(TARGET primordia POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:primordia>/assets)

# GPU feasibility benchmark (not part of the default build): cmake --build build --target gpu_bench
add_executable(gpu_bench EXCLUDE_FROM_ALL tools/gpu_bench.cpp)
target_link_libraries(gpu_bench PRIVATE SDL3::SDL3 PkgConfig::EPOXY)
target_compile_options(gpu_bench PRIVATE -O3 -march=native -fopenmp)
target_link_options(gpu_bench PRIVATE -fopenmp)
